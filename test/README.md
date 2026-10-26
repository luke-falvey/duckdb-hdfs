# Tests

[SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html) and the Docker
cluster used to exercise the `hdfs` extension end-to-end.

## Layout

- `sql/hdfs_no_cluster.test` — checks that need no cluster: settings,
  `hdfs_ls`/`hdfs_stat` schemas, named-parameter binding, and `hdfs_exists`
  error reporting. Runs as part of `make test`.
- `sql/hdfs.test` — integration tests run against a real HDFS. Gated behind
  `require-env HDFS_TEST_RUNNING`, so `make test` skips them unless a cluster is
  up.
- `sql/hdfs_permissions.test` — permission-error handling. Run separately with
  `HADOOP_USER_NAME=guest` (a non-superuser; the main suite's superuser bypasses
  HDFS permission checks and can never see an AccessControlException).
- `sql/hdfs_ez_no_provider.test` — encrypted files without a key provider
  configured (run by `run_hdfs_tests.sh` with a stripped client config).
- `cpp/hdfs_glob_test.cpp`, `cpp/hdfs_crypto_test.cpp` — unit tests for the
  glob engine and the encryption helpers (`make test_unit`).
- `sql/hdfs_kerberos.test`, `sql/hdfs_kerberos_no_ticket.test` — a
  Kerberos-secured cluster, with and without a ticket. Run by
  `scripts/run_kerberos_tests.sh`, which starts `docker-kerberos/` (an MIT KDC
  plus a secured NameNode/DataNode) and logs in as `hadoop@EXAMPLE.TEST`; the
  host-side client config is `hdfs-conf-kerberos/`. The suite runs once per
  wire-protection mode (`docker-kerberos/modes/`: none, integrity, privacy,
  aes, encrypt-key), and checks that the cluster refuses clients without the
  required protection.
- `docker/` — a single-node HDFS (NameNode + DataNode) image + compose file.
- `hdfs-conf/` — host-side Hadoop client config (`core-site.xml` /
  `hdfs-site.xml`) for talking to that cluster. Pointed at via `HADOOP_CONF_DIR`.
- `scripts/` — `hdfs_up.sh`, `hdfs_down.sh`, and `run_hdfs_tests.sh`.

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
`/hidtest`, `/permtest` and the encryption zone `/ez`, runs `test/sql/hdfs.test` (as the superuser) and
`test/sql/hdfs_permissions.test` (as a plain user) with `HDFS_TEST_RUNNING=1`
and `HADOOP_CONF_DIR` pointing at `test/hdfs-conf`, then tears everything down.

To keep the cluster up between runs, use `hdfs_up.sh` / `hdfs_down.sh` directly
(see the project README).
