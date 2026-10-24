# Benchmarks

`run_bench.sh` starts the Docker test cluster (`test/docker`), seeds benchmark
data under `/bench`, and runs a fixed set of queries with `build/release/duckdb`.
For each query it reports the median wall time (one DuckDB process per run, so
connection setup counts) and the NameNode RPCs one run makes, by method, from
the NameNode's per-method RPC counters (JMX; each count waits ~10 s for the
JMX cache, so a run takes a few minutes). RPC counts don't depend on machine
noise, so they are the better signal for changes that remove round trips.

```sh
make
test/bench/run_bench.sh                      # local Docker, no added latency
test/bench/run_bench.sh --latency 1          # +1 ms each way (2 ms round trips)
test/bench/run_bench.sh --save mine          # save results/mine.json
test/bench/run_bench.sh --compare baseline   # side by side with results/baseline.json
test/bench/run_bench.sh --only "glob scan"   # one query
```

`--latency MS` routes NameNode and DataNode traffic through
[toxiproxy](https://github.com/Shopify/toxiproxy), which models a real network.
On localhost a round trip costs ~0.1 ms, which hides per-file round trips;
real clusters are 0.5–5 ms.

Environment: `BENCH_REUSE=1` reuses a running cluster and its seeded data,
`BENCH_KEEP=1` leaves the cluster running, `BENCH_QUICK=1` seeds a smaller
data set. `results/` is ignored by git.

Data set: 2,000 small Parquet files (`/bench/small/p=*/`), one 20M-row
Parquet file, one 8M-row CSV file, and a three-level partitioned tree of
~490 directories (`/bench/tree`).

## Results

### Round trips per file


Docker Desktop on an Apple M-series Mac (10 cores; the HDFS image is amd64
and runs emulated), DuckDB v2.0.0. Median of 5 runs (3 with latency).
"Before" is the first libhdfs3 build; "after" adds the changes below.

| query | before | after | before, 2 ms RTT | after, 2 ms RTT | NameNode RPCs before → after |
|---|---|---|---|---|---|
| glob scan, 2000 small Parquet files | 2.97s | 2.00s | 3.88s | 2.23s | 16006 → 4003 |
| large Parquet scan, 8 threads | 4.68s | 4.68s | 4.67s | 4.60s | 55 → 17 |
| large Parquet scan, 32 threads | 4.64s | 4.77s | 4.70s | 4.78s | 94 → 16 |
| sequential CSV scan | 2.44s | 2.35s | 2.29s | 2.39s | 7 → 2 |
| hdfs_ls wide tree, parallelism 1 | 0.44s | 0.26s | 4.44s | 2.59s | 972 → 486 |
| hdfs_ls wide tree, parallelism 16 | 0.15s | 0.10s | 0.28s | 0.18s | 972 → 486 |
| hdfs_ls wide tree, parallelism 64 | 0.16s | 0.11s | 0.23s | 0.12s | 972 → 486 |
| same small-file scan twice | failed* | 2.79s | 6.76s | 3.35s | 32012 → 8006 |

\* DataNode connect timeouts: the old client opened a new DataNode connection
for nearly every read (~4,400 for this query), more than Docker Desktop's port
forwarding keeps up with.

What changed:

- **Open = 1 RPC** (was `getFileStatus` + `getBlockLocations`): the block
  locations carry the length and encryption info (libhdfs3 patch
  `fewer-round-trips.patch`). Extra pooled streams reuse them.
- **Shared open files**: DuckDB's Parquet reader opens each file twice (metadata,
  then scan) while the first handle is open; the second open reuses it.
- **Cached size/mtime** on the handle, `OpenFileExtended` (sizes and mtimes
  from the glob listing), and `GetVersionTag` for DuckDB's external file cache:
  no `getFileStatus` per file.
- **One `listStatus` per directory** (libhdfs3 listed every directory twice)
  and no `getFsStats` on connect.
- **DataNode connection reuse** (`peer-cache.patch`): libhdfs3 kept one idle
  connection per DataNode for the whole process, so parallel readers opened a
  new TCP connection (and, on secure clusters, ran a SASL handshake) per read.
  Now several per DataNode, per file system, 64 by default.

Tried and dropped (no measurable gain):

- Streaming contiguous reads (sequential `read` instead of `pread`): DuckDB's
  CSV reader reads large buffers, so one DataNode request per buffer costs
  nothing noticeable.
- Hardware CRC32C via Abseil: reading a 724 MB file used 0.19 s of client CPU
  in all, checksums included; the time is in the network and the DataNode.
- A shared listing thread pool: starting threads costs microseconds per
  listing.

Large scans are bound by the (emulated) DataNode, not the client: at 32
threads the client uses ~2.4 s of CPU over 4.7 s.

The RPC counts in this table come from the NameNode audit log (one line per
path), which this benchmark used before it counted calls through JMX.

### Batched listing

Hadoop 3.3+ NameNodes list up to 100 directories per call
(`getBatchedListing`). Same setup; "one per call" sets
`duckdb.hdfs.batched-listing.enabled=false` (via `BENCH_HADOOP_CONF_DIR`).
Query results were identical.

| query | one per call | batched | one per call, 2 ms RTT | batched, 2 ms RTT | NameNode RPCs |
|---|---|---|---|---|---|
| glob only, 2000 partition directories | 0.29s | 0.18s | 0.54s | 0.19s | 2002 → 22 |
| glob scan, 2000 small Parquet files | 2.13s | 1.64s | 2.40s | 1.86s | 4003 → 2023 |
| hdfs_ls wide tree, parallelism 1 | 0.25s | 0.08s | 2.44s | 0.10s | 486 → 8 |
| hdfs_ls wide tree, parallelism 16 | 0.11s | 0.08s | 0.17s | 0.10s | 486 → 12–14 |
| hdfs_ls wide tree, parallelism 64 | 0.11s | 0.08s | 0.14s | 0.10s | 486 → 13–17 |
| same small-file scan twice | 2.90s | 2.63s | 3.63s | 2.83s | 8006 → 4046 |

Queries that read few files (large scans, CSV) are unchanged. The remaining
RPCs of the glob scan are one `getBlockLocations` per file opened. With more
listing threads the batches get smaller (the queued directories are shared
out), which costs a few more calls but no time.
