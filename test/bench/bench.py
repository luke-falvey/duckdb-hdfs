"""Benchmark the hdfs extension against the Docker test cluster.

For each query: the median wall time over N runs (one DuckDB process per run,
so connection setup is included), and the number of NameNode RPCs one run
makes, counted from the NameNode's audit log between two marker calls.

Run through run_bench.sh, which starts the cluster and seeds the data.
Results are printed as a Markdown table and can be saved as JSON and compared
with an earlier run (--save NAME / --compare NAME).
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import time
import uuid

CONTAINER = "duckdb-hdfs-test"
NN = "hdfs://localhost:9000"
RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results")

QUERIES = [
    (
        "glob scan, 2000 small Parquet files",
        "SELECT count(*), sum(v) FROM read_parquet('{nn}/bench/small/*/*.parquet')",
    ),
    (
        "large Parquet scan, 8 threads",
        "SET threads=8; SELECT sum(v), count(DISTINCT s) FROM read_parquet('{nn}/bench/big.parquet')",
    ),
    (
        "large Parquet scan, 32 threads",
        "SET threads=32; SELECT sum(v), count(DISTINCT s) FROM read_parquet('{nn}/bench/big.parquet')",
    ),
    ("sequential CSV scan", "SELECT count(*), sum(v) FROM read_csv('{nn}/bench/big.csv')"),
    (
        "hdfs_ls wide tree, parallelism 1",
        "SET hdfs_list_parallelism=1; SELECT count(*) FROM hdfs_ls('{nn}/bench/tree/**')",
    ),
    (
        "hdfs_ls wide tree, parallelism 16",
        "SET hdfs_list_parallelism=16; SELECT count(*) FROM hdfs_ls('{nn}/bench/tree/**')",
    ),
    (
        "hdfs_ls wide tree, parallelism 64",
        "SET hdfs_list_parallelism=64; SELECT count(*) FROM hdfs_ls('{nn}/bench/tree/**')",
    ),
    (
        "same small-file scan twice (external file cache)",
        "SELECT count(*) FROM read_parquet('{nn}/bench/small/*/*.parquet'); "
        "SELECT count(*), sum(v) FROM read_parquet('{nn}/bench/small/*/*.parquet')",
    ),
]


class QueryFailed(Exception):
    pass


def run_duckdb(duckdb, sql):
    start = time.perf_counter()
    proc = subprocess.run([duckdb, "-no-agent", "-csv", "-noheader", "-c", sql], capture_output=True, text=True)
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        raise QueryFailed(proc.stderr.strip().splitlines()[-1] if proc.stderr.strip() else "exit %d" % proc.returncode)
    return elapsed, proc.stdout.strip()


def audit_lines():
    proc = subprocess.run(["docker", "logs", CONTAINER], capture_output=True, text=True)
    return [line for line in (proc.stdout + proc.stderr).splitlines() if "FSNamesystem.audit" in line]


def count_rpcs(duckdb, sql):
    """NameNode RPCs one run of `sql` makes, by audit-log command."""
    marker = "/__bench_marker__/" + uuid.uuid4().hex
    run_duckdb(duckdb, "SELECT hdfs_exists('%s%s-start')" % (NN, marker))
    run_duckdb(duckdb, sql)
    run_duckdb(duckdb, "SELECT hdfs_exists('%s%s-end')" % (NN, marker))
    # The audit log is written asynchronously; give it a moment.
    for _ in range(20):
        lines = audit_lines()
        starts = [i for i, l in enumerate(lines) if marker + "-start" in l]
        ends = [i for i, l in enumerate(lines) if marker + "-end" in l]
        if starts and ends:
            break
        time.sleep(0.5)
    else:
        return {}
    counts = {}
    for line in lines[starts[-1] + 1 : ends[0]]:
        cmd = next((f[4:] for f in line.split("\t") if f.startswith("cmd=")), "?")
        counts[cmd] = counts.get(cmd, 0) + 1
    return counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", required=True)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--label", default="")
    parser.add_argument("--save", help="save results as results/NAME.json")
    parser.add_argument("--compare", help="compare with results/NAME.json")
    parser.add_argument("--only", help="run only queries whose name contains this text")
    args = parser.parse_args()

    baseline = None
    if args.compare:
        with open(os.path.join(RESULTS_DIR, args.compare + ".json")) as f:
            baseline = {r["query"]: r for r in json.load(f)["results"]}

    results = []
    for name, template in QUERIES:
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
                    "output": output,
                }
            )
        except QueryFailed as e:
            print("\n%s: FAILED: %s" % (name, e), file=sys.stderr)
            results.append(
                {"query": name, "median_s": None, "rpcs": {}, "rpc_total": 0, "output": None, "error": str(e)}
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

    for r in results:
        detail = ", ".join("%s %d" % kv for kv in sorted(r["rpcs"].items()))
        row = "| %s | %s | %d (%s) |" % (r["query"], fmt_time(r["median_s"]), r["rpc_total"], detail)
        if baseline and r["query"] in baseline:
            b = baseline[r["query"]]
            row += " %s | %d |" % (fmt_time(b["median_s"]), b["rpc_total"])
            if None not in (b["output"], r["output"]) and b["output"] != r["output"]:
                row += " RESULT DIFFERS |"
        print(row)

    if args.save:
        os.makedirs(RESULTS_DIR, exist_ok=True)
        with open(os.path.join(RESULTS_DIR, args.save + ".json"), "w") as f:
            json.dump({"label": args.label, "runs": args.runs, "results": results}, f, indent=2)


if __name__ == "__main__":
    main()
