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

## Installing and loading

The extension isn't in DuckDB's core or community extension repositories, so
it is unsigned. Build it (see [Building](#building)), or take
`hdfs.duckdb_extension` for your platform from a CI run's artifacts. It must
match your DuckDB version and platform. Then allow unsigned extensions and
load it:

```sh
duckdb -unsigned        # sets the allow_unsigned_extensions option at startup
```

```sql
LOAD '/path/to/hdfs.duckdb_extension';

-- Or install it once, then load it by name in later sessions:
INSTALL '/path/to/hdfs.duckdb_extension';
LOAD hdfs;
```

From Python: `duckdb.connect(config={'allow_unsigned_extensions': 'true'})`,
then `LOAD`. The shell built by `make` (`build/release/duckdb`) has the
extension linked in and needs no `LOAD`.

**Load it before the first `hdfs://` path.** DuckDB autoloads extensions only
for a fixed list of URL prefixes (`s3://`, `https://`, …), and `hdfs://` isn't
on it. Without `LOAD hdfs`, DuckDB treats an `hdfs://` URL as a local file name,
so a read fails with a misleading `No files found that match the pattern`, and
`hdfs_ls` doesn't exist.

For the same reason DuckDB treats `hdfs://` paths as local in other places.
The one to know about is the external file cache, which keeps HDFS reads only
with `SET cache_local_files = true` (see [Performance](#performance)).

## Platforms

Linux and macOS. CI (`.github/workflows/MainDistributionPipeline.yml`) builds
linux_amd64, linux_arm64 and osx_arm64, and runs the no-cluster SQL tests on
linux_amd64 and osx_arm64 (linux_arm64 is built but not tested). osx_amd64
and the musl builds are opt-in in extension-ci-tools and aren't built. The
cluster and Kerberos tests run on Linux amd64 only
(`.github/workflows/Integration.yml`).
Windows and WebAssembly aren't supported: libhdfs3 needs POSIX sockets, and its
vcpkg port excludes Windows.

## What works

- Reading any file format DuckDB supports (Parquet, CSV, JSON, …) over `hdfs://`.
- Writing via `COPY ... TO`, including `PARTITION_BY`. See [Writing](#writing)
  for what happens when a write fails.
- Globbing, directory listing, file metadata, and directory/file management
  (create/remove directories, remove files, rename).
- Concurrent positional reads (DuckDB's parallel Parquet reader).
- NameNode HA (`dfs.nameservices`, e.g. `hdfs://mycluster/path`) and
  erasure-coded files. *Experimental: no test covers either yet (the test
  cluster has one NameNode and replicated files).*
- Kerberos-secured clusters (`hadoop.security.authentication=kerberos`): the
  identity is the principal in your Kerberos ticket cache (`kinit` first;
  `KRB5CCNAME` or `hadoop.security.kerberos.ticket.cache.path` pick a cache),
  using the system `krb5.conf` or `KRB5_CONFIG`. Block access tokens are
  handled for DataNode reads and writes.
- Wire encryption on Kerberos clusters: NameNode RPC protection
  (`hadoop.rpc.protection` = integrity / privacy, via GSSAPI) and DataNode
  transfer protection (`dfs.data.transfer.protection` = integrity / privacy,
  via DIGEST-MD5 with 3DES or a negotiated AES/CTR cipher), as well as
  `dfs.encrypt.data.transfer`. RPC protection is negotiated automatically
  (restrict it with `hadoop.rpc.protection`); for DataNode protection set
  `dfs.data.transfer.protection` (or `dfs.encrypt.data.transfer`) in the client
  config, as for Hadoop. *Experimental: this code (SASL wrapping, the
  DIGEST-MD5 client and AES/CTR transfer encryption) is patched into libhdfs3
  here and is new. CI runs the Kerberos suite against a single-node Docker
  cluster in each protection mode (`test/scripts/run_kerberos_tests.sh`), but
  it hasn't been verified on production clusters.*
- HDFS encryption zones (Transparent Data Encryption): files are decrypted and
  encrypted client-side with AES-CTR, after the Hadoop KMS unwraps each file's
  key. Configure the key provider as for Hadoop
  (`hadoop.security.key.provider.path`, or `dfs.encryption.key.provider.uri`,
  e.g. `kms://https@kms1;kms2:9600/kms`). The KMS is called with SPNEGO on
  Kerberos clusters, else with simple authentication as the effective user.
  Only `http` and `https` KMS URLs are accepted. The KMS is called with
  libcurl rather than DuckDB's HTTP client (which can't do SPNEGO), so
  DuckDB's `http_proxy` and CA settings don't apply to it: for an `https` KMS,
  the CA bundle comes from `CURL_CA_BUNDLE` / `SSL_CERT_FILE` or the usual
  system locations, and a proxy from libcurl's `https_proxy` / `http_proxy` /
  `no_proxy` environment variables. Decrypted file keys are cached per
  connection for up to 10 minutes, and wiped from memory when they expire or
  the connection closes (at the latest, with the database).

## Not supported yet

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

`hdfs_ls` and `hdfs_stat` return the same columns. Row order is not
guaranteed — add `ORDER BY path` if it matters.

| Column | Type | Description |
|---|---|---|
| `path` | `VARCHAR` | Full URL, `hdfs://host:port/a/b` |
| `name` | `VARCHAR` | Last path component |
| `type` | `VARCHAR` | `'file'` or `'directory'` |
| `size` | `BIGINT` | Length in bytes (0 for directories) |
| `owner`, `group` | `VARCHAR` | |
| `permissions` | `VARCHAR` | `rwxr-xr-x`, as `hdfs dfs -ls` shows it (the sticky bit as `t`/`T`: `rwxrwxrwt`) |
| `mode` | `USMALLINT` | The permission bits as a number, sticky bit included (`rwxr-xr-x` = 493, `rwxrwxrwt` = 1023); HDFS's ACL, encryption and erasure-coding flag bits are left out |
| `replication` | `INTEGER` | `NULL` for directories |
| `block_size` | `BIGINT` | `NULL` for directories |
| `last_modified` | `TIMESTAMP WITH TIME ZONE` | Modification time (an instant, as `read_blob`'s `last_modified`) |
| `last_accessed` | `TIMESTAMP WITH TIME ZONE` | Access time; `NULL` when HDFS doesn't track it (directories, or `dfs.namenode.accesstime.precision = 0`) |

`hdfs_exists` asks the NameNode each time it runs, so a prepared statement
sees files that appeared or disappeared since `PREPARE`. Errors other than
"not found" (permission denied, NameNode unreachable) fail the query.

All three functions obey the same access controls as reads:
`disabled_filesystems = 'HdfsFileSystem'` disables them, and with
`enable_external_access = false` only paths under `allowed_directories` /
in `allowed_paths` are allowed.

Behavior notes:

- **Globs** use DuckDB's syntax (`*`, `?`, `[abc]`, `**` for any depth) plus
  Hadoop-style `{a,b}` alternation, and also work in `read_parquet` /
  `read_csv` / `glob()`. A path with wildcards makes `hdfs_ls` return the
  matched entries themselves (like `ls -d`; no matches → empty result); a
  plain directory path lists its children. Names are matched by DuckDB's own
  glob matcher, so `?` matches one byte (as for local files). In scans,
  globbed files are read in path order.
- **Literal names with glob characters**: a path containing `*`, `?`, `[` or
  `{` is a pattern, in scans and `hdfs_ls` alike. One that matches nothing is
  tried as a literal path, so a file named `data[1].csv` or `a{b}.csv` can be
  read and listed by its name (a literal directory lists its children).
- **Braces**: `{a,b}` groups nest, and a `{` or `,` inside a character class
  (`[{,]`) or escaped (`\{`) is literal. An unclosed group is an error. Each
  group multiplies the number of patterns, and a pattern that expands to more
  than 10,000 is an error.
- **Hidden entries** (names starting with `_` or `.`, e.g. `_SUCCESS`) are
  skipped unless the path names them explicitly (`'/data/_*'`) or you set
  `include_hidden := true` / `SET hdfs_include_hidden = true`.
- **Permission errors** fail the query; `skip_permission_errors := true` (or
  the `SET` variant) prunes unreadable subtrees instead. The root path itself
  still fails.
- **Performance**: globs expand client-side (HDFS has no server-side glob):
  only directories the pattern can match are listed, so anchor patterns
  deep — `/data/2024/*` starts listing at `/data/2024`, while `**` walks the
  whole subtree. On Hadoop 3.3+ NameNodes, up to 100 directories are listed
  per NameNode call (batched listing); older NameNodes and Routers get one
  call per directory. Up to `hdfs_list_parallelism` calls run in parallel.
  Directories are processed a page (1,000 entries) at a time, so `LIMIT` or a
  cancelled query stops a walk after the current page.

## Writing

- **Sequential only.** HDFS files are append-only: each write must start where
  the previous one ended (a write after a seek elsewhere is an error), a file
  can't be opened for reading and writing at once, and appending to an existing
  file is not supported.
- **A failed write leaves no file.** HDFS publishes a file under its name when
  the writer closes it, so a write that fails (an error in the query, a failed
  write or close) is *aborted*: the file is deleted on the NameNode without
  being completed. If that delete fails too (the NameNode is unreachable), the
  error says so; the file then stays under construction until the client that
  wrote it disconnects (`CALL hdfs_reset_connections()`, or the process
  exiting) and its lease expires, when HDFS recovers it with what was written.
  A writer that is dropped without an error commits, as for local files; if
  that commit fails, the file is aborted as above and the error can only be
  logged (`duckdb_logs`, when logging is enabled), since the caller didn't ask
  for it. Core does this for `EXPORT DATABASE`'s `schema.sql` and `load.sql`.
- **Existing files.** DuckDB asks for an exclusive create when it expects a new
  file; the NameNode then refuses to replace a file that appeared meanwhile.
  Otherwise an existing file is replaced.
- **Durability.** A completed file is replicated on the DataNodes, but, as for
  any HDFS client, its last bytes may still be in their page cache unless the
  cluster sets `dfs.datanode.synconclose`. DuckDB's `FileSync` does an hsync:
  what was written so far is on the DataNodes' disks, while the file stays
  open. Because appending isn't supported, DuckDB's file log storage can't
  reopen an existing log file on HDFS (a new session fails to append to it).
- **Directories.** A partitioned `COPY` creates its output directory and the
  partition directories one level at a time, so, as for local files, the
  output directory's parent must exist. Removing a directory that should be
  empty (e.g. after a failed partitioned `COPY`) is one non-recursive delete,
  which the NameNode refuses for a non-empty directory.
- **Connection failures.** Reads are retried once on a new connection. Creates,
  deletes and renames are not simply sent again (the first attempt may have
  taken effect): after reconnecting, the extension checks whether it did (a
  deleted path is gone; a renamed source is gone and the target is the same
  file, by its file id, or by its length without one). Where it can't tell,
  as for a create whose file now exists, the error says the outcome is
  unknown.

## URLs

Paths are `hdfs://host:port/path`; `host:port` selects the NameNode. The
scheme-only form `hdfs:///path` uses `fs.defaultFS` from your Hadoop config.
`hdfs_ls`, `hdfs_stat` and `hdfs_exists` take these URLs only (a bare `/path`
is an error, not a path on the default FS). Paths with a `.` or `..`
component are refused, as HDFS forbids them. In a glob pattern this applies
after brace expansion and escapes too: `{..}` and `\.\.` are refused (so a
file literally named `{..}` is reached as `\{..\}`).

Glob patterns may have at most 128 brace groups and 64 KiB, and expand to at
most 10,000 patterns; expanding one may build at most 16 MiB of patterns,
counting the intermediate ones. An invalid pattern is refused before
connecting.

## Configuration

### Extension settings

| Setting | Default | Description |
|---|---|---|
| `hdfs_list_parallelism` | `16` | Maximum number of concurrent directory-listing calls (threads) in each recursive listing or glob, from 1 to 64. |
| `hdfs_include_hidden` | `false` | Whether listings, globs and scans include hidden entries. |
| `hdfs_skip_permission_errors` | `false` | Whether to skip unreadable subtrees instead of failing the query. |
| `hdfs_batched_listing` | `true` | Whether to list several directories per NameNode call. |

```sql
SET hdfs_list_parallelism = 64;
```

- `hdfs_list_parallelism` multiplies the load on the NameNode: raise it
  cautiously on shared clusters. Each listing or glob starts its own threads,
  outside DuckDB's `threads` limit, so the setting is capped at 64 (0 is an
  error; 1 lists one directory at a time), and concurrent queries each start
  their own.
- Hidden entries are names starting with `_` or `.`. `hdfs_ls` takes the
  per-call override `include_hidden := …`, and `skip_permission_errors := …`
  for `hdfs_skip_permission_errors`.
- Batched listing (`getBatchedListing`, Hadoop 3.3+) lists up to 100
  directories per call. NameNodes without it are detected on the first call,
  so turn it off only if a proxy in front of the NameNode mishandles the call.

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
- **Batched listing**: use the `hdfs_batched_listing` setting. Setting
  `duckdb.hdfs.batched-listing.enabled` to `false` in `hdfs-site.xml` still
  turns it off too, but is deprecated.

```sh
export HADOOP_CONF_DIR=/etc/hadoop/conf
export HADOOP_USER_NAME=analytics   # only on non-secure clusters
```

In an embedded context (e.g. Python), set these before the first HDFS access —
the client is created lazily — or use a secret.

### Secrets

An `hdfs` secret configures the connections to the URLs in its scope (by
default all of `hdfs://`) instead of the environment, as DuckDB's `s3` and
`azure` secrets do:

```sql
CREATE SECRET prod (
    TYPE hdfs,
    SCOPE 'hdfs://prod-nn:8020',
    CONFIG_DIR '/etc/hadoop/prod',  -- core-site.xml / hdfs-site.xml
    USER 'analytics'                -- simple authentication only
);
```

Both parameters are optional; what a secret leaves out comes from the
environment. On a Kerberos cluster the identity is always the ticket cache's
principal, so `USER` is an error there. A directory without `core-site.xml`
or `hdfs-site.xml` is an error.

The client reads `CONFIG_DIR` itself, outside DuckDB's file system. So with
`enable_external_access = false` the directory must be an absolute path (not
`~/...` or `file://...`, since the client reads the path as given) in
`allowed_directories` or `allowed_paths`, as for any local read. An
`allowed_paths` entry for the directory lets the client read the files in it,
though DuckDB's own reads would need each file listed. `HADOOP_CONF_DIR` and
`HADOOP_HOME` are the operator's environment and are not checked. A
`CONFIG_DIR` is trusted configuration: it names NameNodes, the KMS and the
ticket cache, so allow only directories whose files you trust (a query that
can write to an allowed directory can also write a `core-site.xml` there).

#### Configuration snapshots

A `config_snapshot` secret holds the Hadoop configuration itself, as the
properties a `core-site.xml` / `hdfs-site.xml` would set, so a persistent
secret keeps working without any file on the machine:

```sql
CREATE PERSISTENT SECRET prod (
    TYPE hdfs,
    PROVIDER config_snapshot,
    SCOPE 'hdfs://prod-ns/',
    PROPERTIES MAP {
        'fs.defaultFS': 'hdfs://prod-ns',
        'dfs.nameservices': 'prod-ns',
        'dfs.ha.namenodes.prod-ns': 'nn1,nn2',
        'dfs.namenode.rpc-address.prod-ns.nn1': 'nn1.example.com:8020',
        'dfs.namenode.rpc-address.prod-ns.nn2': 'nn2.example.com:8020',
        'dfs.client.failover.proxy.provider.prod-ns':
            'org.apache.hadoop.hdfs.server.namenode.ha.ConfiguredFailoverProxyProvider'
    },
    USER 'analytics'                -- optional; simple authentication only
);
```

- The properties are the whole configuration: no XML file and no
  `HADOOP_CONF_DIR` is read. `HADOOP_USER_NAME` still applies without `USER`,
  and on a Kerberos cluster the identity is still the local ticket cache's
  principal (the ticket and `krb5.conf` stay on the machine).
- Values are stored as given (no `${...}` substitution, as libhdfs3 does with
  XML files). The extension's defaults (timeouts, retries) apply on top, as
  for `CONFIG_DIR`.
- Credentials are refused: a property whose name contains `password`,
  `passwd`, `secret` or `keytab`, or starts with
  `hadoop.security.credential.`, is an error. Secrets are stored unencrypted,
  and nothing is redacted in `duckdb_secrets()`.
- The properties can't be set while `enable_external_access = false` (they
  name NameNodes, the KMS and local paths); a snapshot created before external
  access was disabled keeps working.
- A snapshot doesn't change when the cluster does: to pick up a new
  configuration, `CREATE OR REPLACE` the secret. A stored secret whose
  properties were edited by hand is refused.

- **Scopes match whole path components**: a secret scoped to
  `hdfs://nn:8020/data` covers `hdfs://nn:8020/data` and everything below it,
  but not `hdfs://nn:8020/data2`. The longest matching scope wins, as for other
  DuckDB secrets.
- **Each path resolves its own secret.** A rename (`COPY ... USE_TMP_FILE`)
  needs its source and target to resolve to the same configuration.
- **Connections are cached** per NameNode and configuration (`CONFIG_DIR` and
  `USER`), for the database's lifetime: secrets that configure a NameNode the
  same way share one connection (a secret without parameters shares the one
  URLs without a secret use). Replacing a secret (`CREATE OR REPLACE SECRET`)
  with a new configuration replaces its connection. To pick up other changes
  (an edited `core-site.xml` / `hdfs-site.xml`, a new Kerberos ticket cache,
  environment variables) or to release the connections of dropped secrets,
  call `hdfs_reset_connections()`; it returns how many connections it dropped.
  Files that are open keep their connection until they are closed.
- **Persistent secrets** (`CREATE PERSISTENT SECRET`) are stored in
  `~/.duckdb/stored_secrets` (DuckDB's `secret_directory`), unencrypted. A
  persistent `hdfs` secret whose provider this version doesn't know (one
  written by a newer version of the extension) is an error for the URLs in
  its scope: update the extension, or `DROP PERSISTENT SECRET` it.

```sql
CALL hdfs_reset_connections();
```

## Performance

On a real cluster each NameNode or DataNode round trip costs 0.5–5 ms, so
scans of many small files are dominated by round trips, not bandwidth. The
extension keeps them down:

- **Opening a file takes one NameNode RPC once its status is known**: its
  block locations, which also carry the length and any encryption info. The
  status (size, modification time) of a file found by a glob or listing is
  reused; otherwise it costs one more RPC. So reading a single file, e.g.
  `read_parquet('hdfs://nn/f.parquet')`, is two RPCs: DuckDB expands even a
  literal path as a glob, which stats it, then the open fetches the block
  locations. A file opened again while it is still open (DuckDB's Parquet
  reader does this) shares the open file if it is still the same version (file
  id, modification time and length), and extra streams for parallel reads of
  one file cost no RPC.
- **DataNode connections are reused** across reads and files: up to
  `dfs.client.socketcache.capacity` idle connections (default 64 here, 16 in
  Hadoop; DuckDB reads on many threads at once) are kept for
  `dfs.client.socketcache.expiryMsec` (default 3000).
- **Listings are batched and run in parallel**: on Hadoop 3.3+ one NameNode
  call lists up to 100 directories (`getBatchedListing`, capped by the
  NameNode's `dfs.batched.ls.limit`), and up to `hdfs_list_parallelism` calls
  run at once. NameNodes without batched listing (older than 3.3, or a
  Router) are detected on the first call and listed one directory per call.
  Anchor glob patterns as deep as you can (`/data/2024/*/*.parquet` rather
  than `/data/**`): only the directories a pattern can match are listed.

A scan of 2,000 small Parquet files, one per partition directory, makes about
one NameNode RPC per file (opening it; the 2,000 directories are listed in 22
calls) where it made 8. `test/sql/hdfs_rpc_log.test` pins the RPC counts of a
single-file read and of listings.

### Logging NameNode RPCs

The `HDFS` log type records one entry per NameNode operation the extension
starts (libhdfs3's own calls are not included; see below): the operation (`getFileInfo`, `getBlockLocations`, `listDirectory`,
`getBatchedListing`, `create`, `mkdirs`, `delete`, `rename2`), the HDFS path,
`duration_us` and whether it succeeded (`ok`).

```sql
CALL enable_logging('HDFS');
SELECT count(*) FROM read_parquet('hdfs://namenode:8020/data/events/*.parquet');
SELECT op, count(*), sum(duration_us) / 1000 AS ms
FROM duckdb_logs_parsed('HDFS') GROUP BY op;
```

- A `listDirectory` entry covers a whole directory: one directory of more than
  1,000 entries (the NameNode's `dfs.ls.limit`) takes one call per 1,000, and
  `duration_us` is their sum. A `getBatchedListing` entry is one call (its
  `path` names the first of its directories).
- Calls libhdfs3 makes on its own are not logged: further block locations
  while reading a large file, completing a written file, lease renewals and
  KMS calls.
- libhdfs3's own log is process-wide and goes to stderr, so it is kept at
  `FATAL`. To see it while debugging (retries, failovers), set
  `dfs.client.log.severity` (e.g. `INFO`) in `hdfs-site.xml`.

Tuning:

- **Repeated scans**: DuckDB's external file cache can keep file contents in
  memory across queries, validated by the file's id, modification time and
  size. DuckDB only treats a fixed list of URL schemes (`s3://`, `https://`,
  …) as remote, and `hdfs://` isn't on it, so HDFS files are cached only with
  `SET cache_local_files = true` (which caches local files too), and
  `validate_external_file_cache = 'VALIDATE_REMOTE'` doesn't validate them.
  With `SET parquet_metadata_cache = true`, listings carry the same version
  as an etag, so a repeated `count(*)` over a glob is answered from the
  cached footers without opening any file after the first (the one the
  query's bind reads). A single file is opened either way; its cached footer
  is checked through the open handle's tag. Without a file id (an old
  NameNode) there is no version tag, and both caches fall back to the
  modification time.
- **Short-circuit local reads**: when DuckDB runs on DataNode hosts, set
  `dfs.client.read.shortcircuit=true` and `dfs.domain.socket.path` in
  `hdfs-site.xml` (as for Hadoop clients) to read local block replicas
  directly from disk.
- **Large listings**: raise `hdfs_list_parallelism` for wide trees on a
  NameNode that can take it.

`test/bench/` has a benchmark that reports wall times and NameNode RPC counts,
optionally with simulated network latency; see
[test/bench/README.md](test/bench/README.md) for how to run it and recent
numbers.

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
make test_unit    # C++ unit tests (after make)
```

The HDFS integration tests (`test/sql/hdfs.test`, `test/sql/hdfs_glob.test`, …) run against a real
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

The Kerberos suite runs the same way with `test/scripts/run_kerberos_tests.sh`.
CI runs all of these on Linux (`.github/workflows/Integration.yml`); see
[test/README.md](test/README.md) for what each test file covers and
[CONTRIBUTING.md](CONTRIBUTING.md) for how to contribute.

The test cluster uses 1 MiB blocks, and the client fetches the locations of
only 2 blocks on open, so modest test files cross block boundaries and the
prefetched locations (`test/sql/hdfs_edge.test`).

See `test/docker/` for the cluster definition. The one detail that makes HDFS
reachable from the host is advertising the DataNode as `localhost` and setting
`dfs.client.use.datanode.hostname=true` on the client
(`test/hdfs-conf/hdfs-site.xml`).

## Layout

- `src/` — DuckDB extension (C++): `hdfs_extension.cpp` registers the filesystem
  and settings; `hdfs_filesystem.cpp` implements the `FileSystem`;
  `hdfs_functions.cpp` implements the metadata SQL functions.
- `src/hdfs_client.cpp` — the HDFS client on top of libhdfs3, behind the C++
  interface in `src/include/hdfs_client.hpp` (the only file that includes
  libhdfs3 headers): connections, reads, writes, and the parallel streaming
  directory walk. `src/hdfs_glob.cpp` plans glob walks; `src/hdfs_crypto.cpp`
  handles encryption zones.
- `vcpkg_ports/libhdfs3/` — vcpkg port that builds libhdfs3 (pinned commit,
  our own CMake build, and small patches). `vcpkg_ports/gsasl/` builds gsasl
  from its release tarball, so CI images don't need extra autotools packages.
- `test/` — SQL tests, Docker cluster, and helper scripts.
- `docs/` — [UPDATING.md](docs/UPDATING.md) (moving to a new DuckDB or libhdfs3, and the patch inventory) and
  [PLAN.md](docs/PLAN.md) (remaining work and design decisions).

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
