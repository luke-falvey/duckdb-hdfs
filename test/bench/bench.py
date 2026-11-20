"""Benchmark the hdfs extension against the Docker test cluster.

For each query: the median wall time over N runs (one DuckDB process per run,
so connection setup is included), and the NameNode RPCs one run makes, by
method, from the NameNode's per-method RPC counters (JMX). The audit log can't
count calls: it logs each path of a batched listing call separately.

Run through run_bench.sh, which starts the cluster and seeds the data.
Results are printed as a Markdown table and can be saved as JSON and compared
with an earlier run (--save NAME / --compare NAME). Exits non-zero if a query
fails, if --check is given and a query makes more NameNode RPCs than its
ceiling, or if --compare finds a different result or clearly more RPCs.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import time
import urllib.request

NN = "hdfs://localhost:9000"
RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results")

# (name, SQL, ceiling): the ceiling is the most NameNode RPCs one run may make
# with the default client config (batched listing on) before --check fails it.
# They catch structural regressions (an extra RPC per file or per directory),
# not small changes; the counts measured are in README.md.
QUERIES = [
    (
        "glob only, 2000 partition directories",
        "SELECT count(*) FROM glob('{nn}/bench/small/*/*.parquet')",
        60,
    ),
    (
        # One getBlockLocations per file, plus the listing.
        "glob scan, 2000 small Parquet files",
        "SELECT count(*), sum(v) FROM read_parquet('{nn}/bench/small/*/*.parquet')",
        2100,
    ),
    (
        "large Parquet scan, 8 threads",
        "SET threads=8; SELECT sum(v), count(DISTINCT s) FROM read_parquet('{nn}/bench/big.parquet')",
        40,
    ),
    (
        "large Parquet scan, 32 threads",
        "SET threads=32; SELECT sum(v), count(DISTINCT s) FROM read_parquet('{nn}/bench/big.parquet')",
        40,
    ),
    ("sequential CSV scan", "SELECT count(*), sum(v) FROM read_csv('{nn}/bench/big.csv')", 5),
    (
        # ~490 directories: one call per directory would be ~490.
        "hdfs_ls wide tree, parallelism 1",
        "SET hdfs_list_parallelism=1; SELECT count(*) FROM hdfs_ls('{nn}/bench/tree/**')",
        20,
    ),
    (
        "hdfs_ls wide tree, parallelism 16",
        "SET hdfs_list_parallelism=16; SELECT count(*) FROM hdfs_ls('{nn}/bench/tree/**')",
        150,
    ),
    (
        "hdfs_ls wide tree, parallelism 64",
        "SET hdfs_list_parallelism=64; SELECT count(*) FROM hdfs_ls('{nn}/bench/tree/**')",
        150,
    ),
    (
        # DuckDB counts hdfs:// as local, so its external file cache keeps HDFS
        # reads only with cache_local_files. The ceiling is two uncached scans.
        "same small-file scan twice (external file cache)",
        "SET enable_external_file_cache=true; SET cache_local_files=true; "
        "SELECT count(*) FROM read_parquet('{nn}/bench/small/*/*.parquet'); "
        "SELECT count(*), sum(v) FROM read_parquet('{nn}/bench/small/*/*.parquet')",
        4200,
    ),
]

# With --compare: an RPC total above baseline * RPC_TOLERANCE + RPC_SLACK is a regression.
RPC_TOLERANCE = 1.1
RPC_SLACK = 5


class QueryFailed(Exception):
    pass


def run_duckdb(duckdb, sql):
    start = time.perf_counter()
    proc = subprocess.run([duckdb, "-no-agent", "-csv", "-noheader", "-c", sql], capture_output=True, text=True)
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        raise QueryFailed(proc.stderr.strip().splitlines()[-1] if proc.stderr.strip() else "exit %d" % proc.returncode)
    return elapsed, proc.stdout.strip()


JMX = "http://localhost:9870/jmx?qry=Hadoop:service=NameNode,name=RpcDetailedActivityForPort9000"
# DataNode-to-NameNode calls (heartbeats, block reports) share the port; they
# aren't the query's.
DATANODE_METHODS = {
    "BlockReceivedAndDeleted",
    "BlockReport",
    "CacheReport",
    "CommitBlockSynchronization",
    "ErrorReport",
    "RegisterDatanode",
    "ReportBadBlocks",
    "SendHeartbeat",
    "SendLifeline",
    "VersionRequest",
}


def rpc_counters():
    """Client RPCs served so far, by method. JMX values are cached for ~10 s,
    so this first waits that out."""
    time.sleep(11)
    with urllib.request.urlopen(JMX, timeout=10) as response:
        bean = json.load(response)["beans"][0]
    counts = {}
    for key, value in bean.items():
        method = key[: -len("NumOps")]
        # "Overall<Method>NumOps" repeats each method's count.
        if key.endswith("NumOps") and not method.startswith("Overall") and method not in DATANODE_METHODS:
            counts[method[0].lower() + method[1:]] = value
    return counts


def count_rpcs(duckdb, sql):
    """NameNode RPCs one run of `sql` makes, by method."""
    before = rpc_counters()
    run_duckdb(duckdb, sql)
    after = rpc_counters()
    return {m: n - before.get(m, 0) for m, n in after.items() if n - before.get(m, 0) > 0}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", required=True)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--label", default="")
    parser.add_argument("--save", help="save results as results/NAME.json")
    parser.add_argument("--compare", help="compare with results/NAME.json")
    parser.add_argument("--only", help="run only queries whose name contains this text")
    parser.add_argument(
        "--check", action="store_true", help="fail if a query makes more NameNode RPCs than its ceiling"
    )
    args = parser.parse_args()

    baseline = None
    if args.compare:
        with open(os.path.join(RESULTS_DIR, args.compare + ".json")) as f:
            baseline = {r["query"]: r for r in json.load(f)["results"]}

    results = []
    for name, template, ceiling in QUERIES:
        if args.only and args.only not in name:
            continue
        sql = template.format(nn=NN)
        try:
            _, output = run_duckdb(args.duckdb, sql)  # warm-up, and the result to check
            times = [run_duckdb(args.duckdb, sql)[0] for _ in range(args.runs)]
            rpcs = count_rpcs(args.duckdb, sql)
            results.append(
                {
                    "query": name,
                    "median_s": statistics.median(times),
                    "rpcs": rpcs,
                    "rpc_total": sum(rpcs.values()),
                    "rpc_ceiling": ceiling,
                    "output": output,
                }
            )
        except QueryFailed as e:
            print("\n%s: FAILED: %s" % (name, e), file=sys.stderr)
            results.append(
                {
                    "query": name,
                    "median_s": None,
                    "rpcs": {},
                    "rpc_total": 0,
                    "rpc_ceiling": ceiling,
                    "output": None,
                    "error": str(e),
                }
            )
        print(".", end="", flush=True, file=sys.stderr)
    print(file=sys.stderr)

    header = "| query | median | NameNode RPCs |"
    if baseline:
        header += " baseline | baseline RPCs |"
    print("\n" + (args.label + "\n\n" if args.label else "") + header)
    print("|" + "---|" * (header.count("|") - 1))

    def fmt_time(t):
        return "failed" if t is None else "%.2fs" % t

    problems = []
    for r in results:
        detail = ", ".join("%s %d" % kv for kv in sorted(r["rpcs"].items()))
        row = "| %s | %s | %d (%s) |" % (r["query"], fmt_time(r["median_s"]), r["rpc_total"], detail)
        if "error" in r:
            problems.append("%s: failed: %s" % (r["query"], r["error"]))
        elif args.check and r["rpc_total"] > r["rpc_ceiling"]:
            problems.append("%s: %d NameNode RPCs, ceiling %d" % (r["query"], r["rpc_total"], r["rpc_ceiling"]))
        if baseline and r["query"] in baseline:
            b = baseline[r["query"]]
            row += " %s | %d |" % (fmt_time(b["median_s"]), b["rpc_total"])
            if None not in (b["output"], r["output"]) and b["output"] != r["output"]:
                row += " RESULT DIFFERS |"
                problems.append("%s: result differs from the baseline" % r["query"])
            if b["median_s"] is not None and r["rpc_total"] > b["rpc_total"] * RPC_TOLERANCE + RPC_SLACK:
                row += " MORE RPCS |"
                problems.append("%s: %d NameNode RPCs, baseline %d" % (r["query"], r["rpc_total"], b["rpc_total"]))
        print(row)

    if args.save:
        os.makedirs(RESULTS_DIR, exist_ok=True)
        with open(os.path.join(RESULTS_DIR, args.save + ".json"), "w") as f:
            json.dump({"label": args.label, "runs": args.runs, "results": results}, f, indent=2)

    if problems:
        print("\nFAILED:\n" + "\n".join("- " + p for p in problems), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
