# DuckDB HDFS extension

`hdfs` adds a native HDFS filesystem to DuckDB: read and write `hdfs://` URLs
directly — no JVM, no `libhdfs`. It is backed by the native C++
[`libhdfs3`](https://github.com/ClickHouse/libhdfs3) client (ClickHouse's
maintained fork), which speaks the Hadoop RPC and DataNode protocols itself.

```sql
-- Read
SELECT * FROM 'hdfs://namenode:8020/data/events/*.parquet';
SELECT * FROM read_csv('hdfs://namenode:8020/data/users.csv');

-- Write
COPY (SELECT * FROM big_table) TO 'hdfs://namenode:8020/out/table.parquet' (FORMAT parquet);
```

## What works

- Reading any file format DuckDB supports (Parquet, CSV, JSON, …) over `hdfs://`.
- Writing via `COPY ... TO`, including `PARTITION_BY` (HDFS is append-only;
  random writes are not supported).
- Globbing, directory listing, file metadata, and directory/file management
  (create/remove directories, remove files, rename).
- Concurrent positional reads (DuckDB's parallel Parquet reader).
- NameNode HA (`dfs.nameservices`, e.g. `hdfs://mycluster/path`) and
  erasure-coded files.
- Kerberos-secured clusters (`hadoop.security.authentication=kerberos`): the
  identity is the principal in your Kerberos ticket cache (`kinit` first;
  `KRB5CCNAME` or `hadoop.security.kerberos.ticket.cache.path` pick a cache),
  using the system `krb5.conf` or `KRB5_CONFIG`. Block access tokens are
  handled for DataNode reads and writes.
- HDFS encryption zones (Transparent Data Encryption): files are decrypted and
  encrypted client-side with AES-CTR, after the Hadoop KMS unwraps each file's
  key. Configure the key provider as for Hadoop
  (`hadoop.security.key.provider.path`, or `dfs.encryption.key.provider.uri`,
  e.g. `kms://https@kms1;kms2:9600/kms`). The KMS is called with SPNEGO on
  Kerberos clusters, else with simple authentication as the effective user.
  For an `https` KMS, the CA bundle comes from `CURL_CA_BUNDLE` /
  `SSL_CERT_FILE` or the usual system locations.

## Not supported yet

- **Wire encryption** on Kerberos clusters (`hadoop.rpc.protection` =
  integrity/privacy, `dfs.data.transfer.protection`, `dfs.encrypt.data.transfer`).
- **Keytab login and impersonation**: get a ticket with `kinit` (or `kinit -kt`);
  `HADOOP_PROXY_USER` on a Kerberos cluster is an error.
- The SM4 encryption cipher suite (such files fail with an error).

## Listing and metadata

Three read-only SQL functions expose HDFS metadata. Unlike DuckDB's built-in
`glob()`, they include directories and return full `FileStatus` metadata.

```sql
-- List a directory's children.
SELECT name, type, size, owner, permissions
FROM hdfs_ls('hdfs://namenode:8020/data');

-- Recurse with '**', or glob — matched entries come back as rows.
SELECT * FROM hdfs_ls('hdfs://namenode:8020/data/**');
SELECT * FROM hdfs_ls('hdfs://namenode:8020/data/**/*.parquet');

-- Metadata for a single path, as one row.
SELECT * FROM hdfs_stat('hdfs://namenode:8020/data');

-- Existence check (scalar boolean).
SELECT hdfs_exists('hdfs://namenode:8020/data/events');
```

`hdfs_ls` and `hdfs_stat` return the same columns: `path`, `name`, `type`
(`'file'`/`'directory'`), `size`, `owner`, `group`, `permissions`
(`rwxr-xr-x`), `mode`, `replication`, `block_size`, `last_modified`,
`last_accessed`. Row order is not guaranteed — add `ORDER BY path` if it
matters.

Behavior notes:

- **Globs** use DuckDB's syntax (`*`, `?`, `[abc]`, `**` for any depth) plus
  Hadoop-style `{a,b}` alternation, and also work in `read_parquet` /
  `read_csv` / `glob()`. A path with wildcards makes `hdfs_ls` return the
  matched entries themselves (like `ls -d`; no matches → empty result); a
  plain directory path lists its children.
- **Hidden entries** (names starting with `_` or `.`, e.g. `_SUCCESS`) are
  skipped unless the path names them explicitly (`'/data/_*'`) or you set
  `include_hidden := true` / `SET hdfs_include_hidden = true`.
- **Permission errors** fail the query; `skip_permission_errors := true` (or
  the `SET` variant) prunes unreadable subtrees instead. The root path itself
  still fails.
- **Performance**: globs expand client-side, one listing RPC per candidate
  directory (up to `hdfs_list_parallelism` in parallel), so anchor patterns
  deep — `/data/2024/*` starts listing at `/data/2024`, while `**` walks the
  whole subtree.

## URLs

Paths are `hdfs://host:port/path`; `host:port` selects the NameNode. The
scheme-only form `hdfs:///path` uses `fs.defaultFS` from your Hadoop config.

## Configuration

### Extension settings

| Setting | Default | Description |
|---|---|---|
| `hdfs_list_parallelism` | `16` | Max concurrent directory-listing RPCs for recursive listings and globs. Raise cautiously on shared clusters — it multiplies NameNode load. |
| `hdfs_include_hidden` | `false` | Include hidden (`_`/`.`) entries in listings, globs and scans. Per-call override: `include_hidden := …`. |
| `hdfs_skip_permission_errors` | `false` | Skip unreadable subtrees instead of failing the query. Per-call override: `skip_permission_errors := …`. |

```sql
SET hdfs_list_parallelism = 64;
```

### Connection configuration

Connection config comes from the standard Hadoop environment, as for the
Hadoop CLI tools:

- **Cluster config** (`core-site.xml` / `hdfs-site.xml`): `HADOOP_CONF_DIR`,
  or failing that `HADOOP_HOME/etc/hadoop`.
- **Effective user**: `HADOOP_USER_NAME` (or `HADOOP_PROXY_USER`), else the
  current OS account. On a Kerberos cluster it is the ticket cache's principal.
- **Kerberos**: MIT krb5 settings from `/etc/krb5.conf` or `KRB5_CONFIG`. If a
  NameNode is addressed by a short host name (no dots), set
  `qualify_shortname = ""` in `[libdefaults]`, or krb5 asks for a service
  principal with your DNS search domain appended.

```sh
export HADOOP_CONF_DIR=/etc/hadoop/conf
export HADOOP_USER_NAME=analytics   # only on non-secure clusters
```

In an embedded context (e.g. Python), set these before the first HDFS access —
the client is created lazily.

## Building

Targets DuckDB v2.0.0 (the commit pinned by the `duckdb` submodule). Needs a
C++17 compiler, CMake, and [vcpkg](https://vcpkg.io) for the HDFS client and
its dependencies (protobuf, libxml2, gsasl), plus `make` and `pkg-config`.
vcpkg builds libhdfs3 and gsasl from the overlay ports in `vcpkg_ports/`.

```sh
git submodule update --init --recursive
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=~/vcpkg/scripts/buildsystems/vcpkg.cmake
```

```sh
make            # builds libhdfs3 (via vcpkg) + the extension + duckdb
```

DuckDB v2 no longer derives its version from `git describe`, so a plain local
build reports `v2.0.0-devN`. A loadable extension built that way only loads
into that same build. To build one that loads into a stock DuckDB v2.0.0, set
the version explicitly (CI does this automatically):

```sh
DUCKDB_VERSION=v2.0.0 make
```

Artifacts:

- `build/release/duckdb` — shell with the extension statically linked.
- `build/release/extension/hdfs/hdfs.duckdb_extension` — loadable extension.
- `build/release/test/unittest` — DuckDB test runner.

## Testing

Tests that don't need a cluster run with:

```sh
make test         # SQL tests (cluster tests are skipped)
make test_unit    # unit tests: glob engine and encryption helpers
```

The HDFS integration tests (`test/sql/hdfs.test`) run against a real
single-node HDFS in Docker, gated behind `require-env HDFS_TEST_RUNNING`. To
run them end-to-end (requires Docker):

```sh
make                              # ensure duckdb + extension are built
test/scripts/run_hdfs_tests.sh    # builds image, starts HDFS, runs tests, tears down
```

Or manage the cluster manually:

```sh
test/scripts/hdfs_up.sh           # build image, start HDFS, seed fixtures under /test
HDFS_TEST_RUNNING=1 HADOOP_CONF_DIR="$(pwd)/test/hdfs-conf" HADOOP_USER_NAME=hadoop \
    build/release/test/unittest test/sql/hdfs.test
test/scripts/hdfs_down.sh
```

See `test/docker/` for the cluster definition. The one detail that makes HDFS
reachable from the host is advertising the DataNode as `localhost` and setting
`dfs.client.use.datanode.hostname=true` on the client
(`test/hdfs-conf/hdfs-site.xml`).

## Layout

- `src/` — DuckDB extension (C++): `hdfs_extension.cpp` registers the filesystem
  and settings; `hdfs_filesystem.cpp` implements the `FileSystem`;
  `hdfs_functions.cpp` implements the metadata SQL functions.
- `src/hdfs_bridge.cpp` — the HDFS client layer on top of libhdfs3, behind the
  small C API in `src/include/hdfs_bridge.h` (the only file that includes
  libhdfs3 headers): connections, reads, writes, and the parallel streaming
  directory walk. `src/hdfs_glob.cpp` is the glob engine.
- `vcpkg_ports/libhdfs3/` — vcpkg port that builds libhdfs3 (pinned commit,
  our own CMake build, and small patches). `vcpkg_ports/gsasl/` builds gsasl
  from its release tarball, so CI images don't need extra autotools packages.
- `test/` — SQL tests, Docker cluster, and helper scripts.

## Third-party code

The extension statically links:
[libhdfs3](https://github.com/ClickHouse/libhdfs3) (Apache-2.0),
[protobuf](https://github.com/protocolbuffers/protobuf) and
[Abseil](https://github.com/abseil/abseil-cpp) (BSD-3-Clause / Apache-2.0),
[libxml2](https://gitlab.gnome.org/GNOME/libxml2) (MIT), zlib (zlib),
[OpenSSL](https://www.openssl.org) (Apache-2.0),
[libcurl](https://curl.se) (curl license), and
[GNU SASL](https://www.gnu.org/software/gsasl/) (LGPL-2.1-or-later). Static
linking of an LGPL library carries obligations (e.g. allowing relinking);
review them before redistributing binaries.
