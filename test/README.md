# Tests

[SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html) and the Docker
cluster used to exercise the `hdfs` extension end-to-end.

## Layout

- `sql/hdfs_no_cluster.test` — checks that need no cluster: settings,
  `hdfs_ls`/`hdfs_stat` schemas, named-parameter binding, and `hdfs_exists`
  error reporting. Runs as part of `make test`.
- `sql/hdfs_access.test` — `enable_external_access`, `allowed_directories`
  and `allowed_paths` for the metadata functions and scans, hdfs:// URLs only,
  and `.`/`..` components. No cluster; runs in `make test` (its own file,
  because external access can't be turned back on).
- `sql/hdfs_disabled_filesystems.test` — `disabled_filesystems`. No cluster;
  `make test` (its own file, because a disabled file system can't be
  re-enabled).
- `sql/hdfs_connections.test` — secret scope matching by whole path components,
  connection replacement when a secret changes, `hdfs_reset_connections()`. No
  cluster; `make test`.
- `sql/hdfs.test` — integration tests run against a real HDFS. Gated behind
  `require-env HDFS_TEST_RUNNING`, so `make test` skips them unless a cluster is
  up. The tests that need the parquet extension come last, after a late
  `require parquet`, so the rest still run without it.
- `sql/hdfs_edge.test` — boundaries: empty and 1-byte files, reads and writes
  on either side of a block boundary and of the block locations prefetched on
  open, multi-block files plain and in the encryption zone (with
  `run_hdfs_tests.sh` checking that the Java client decrypts DuckDB's writes),
  and overwrites. Its fixtures are under `/edge` and `/ez/edge`.
- `sql/hdfs_permissions.test` — permission-error handling. Run separately with
  `HADOOP_USER_NAME=guest` (a non-superuser; the main suite's superuser bypasses
  HDFS permission checks and can never see an AccessControlException).
- `sql/hdfs_ez_no_provider.test` — encrypted files without a key provider
  configured (run by `run_hdfs_tests.sh` with a stripped client config).
- `sql/hdfs_rpc_log.test` — the `HDFS` log type: the exact NameNode RPCs of a
  single-file read, a metadata-only read, listings, the glob literal fallback
  and a failed write. Cluster; `run_hdfs_tests.sh`.
- `sql/hdfs_glob.test` — glob semantics (wildcards, classes, `**`, braces,
  hidden names) against a fixture tree it creates under `/globtest`.
- `cpp/hdfs_glob_test.cpp`, `cpp/hdfs_functions_test.cpp`,
  `cpp/hdfs_crypto_test.cpp`, `cpp/hdfs_batch_test.cpp` — C++ unit tests for
  the glob planner (brace expansion and its bounds, roots, hidden names,
  `**`), the metadata time and permission helpers, the encryption helpers and
  batched-listing rejection handling (`make test_unit`, after `make`).
- `sql/hdfs_kerberos.test`, `sql/hdfs_kerberos_no_ticket.test` — a
  Kerberos-secured cluster, with and without a ticket. Run by
  `scripts/run_kerberos_tests.sh`, which starts `docker-kerberos/` (an MIT KDC
  plus a secured NameNode/DataNode) and logs in as `hadoop@EXAMPLE.TEST`; the
  host-side client config is `hdfs-conf-kerberos/`. The suite runs once per
  wire-protection mode (`docker-kerberos/modes/`: none, integrity, privacy,
  aes, encrypt-key), and checks that the cluster refuses clients without the
  required protection.
- `docker/` — a single-node HDFS (NameNode + DataNode + KMS) image + compose
  file. Blocks are 1 MiB (`dfs.blocksize`), so small files span several.
- `hdfs-conf/` — host-side Hadoop client config (`core-site.xml` /
  `hdfs-site.xml`) for talking to that cluster. Pointed at via `HADOOP_CONF_DIR`.
  It sets the same 1 MiB block size for DuckDB's writes and fetches the
  locations of only 2 blocks on open (`dfs.prefetchsize`), so reads past 2 MiB
  fetch more.
- `scripts/` — `hdfs_up.sh`, `hdfs_down.sh`, `run_hdfs_tests.sh` and
  `run_kerberos_tests.sh`.
- `bench/` — a benchmark of wall times and NameNode RPC counts; see
  [bench/README.md](bench/README.md).

## Running

Without a cluster (unit/SQL tests only):

```bash
make test
```

Full integration run (requires Docker; needs `make` first so the binaries
exist):

```bash
test/scripts/run_hdfs_tests.sh
```

This builds the HDFS image, starts the cluster, seeds fixtures under `/test`,
`/hidtest`, `/permtest`, `/edge` and the encryption zone `/ez`, runs
`test/sql/hdfs.test`, `hdfs_glob.test`, `hdfs_edge.test` and
`hdfs_rpc_log.test` (as the superuser) and `test/sql/hdfs_permissions.test`
(as a plain user) with `HDFS_TEST_RUNNING=1` and `HADOOP_CONF_DIR` pointing at
`test/hdfs-conf`, then a few checks outside SQL (a same-size replacement isn't
served from the external file cache; batched and unbatched listings agree;
the Java client decrypts what DuckDB encrypted) and
`hdfs_ez_no_provider.test`, and tears everything down. It fails if a
`test/sql` file that requires `HDFS_TEST_RUNNING` wasn't run, so a new cluster
test can't be skipped silently: add it to the script.

The Kerberos suite: `test/scripts/run_kerberos_tests.sh [mode...]`.

CI (`.github/workflows/Integration.yml`, Linux amd64) runs `make test_unit`,
`make test`, both scripts, and `test/bench/run_bench.sh --check` for every pull
request and for pushes to `main` and `duckdb-review` (docs-only changes skip
it).

To keep the cluster up between runs, use `hdfs_up.sh` / `hdfs_down.sh` directly
(see the project README).
