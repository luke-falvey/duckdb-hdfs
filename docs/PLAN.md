# Plan: aligning duckdb-hdfs with DuckDB

This file lists the work left over from the DuckDB alignment changes, and the decisions taken along the way. The
work so far (bug fixes, DuckDB integration, the C++ client interface, tests and `hdfs` secrets) is described in the
[README](../README.md), [docs/UPDATING.md](UPDATING.md) and [test/README.md](../test/README.md).

## Remaining work

Ordered by priority. Each item is meant to be one small pull request.

### 1. Verify the libhdfs3 security fixes on a secured cluster

`vcpkg_ports/libhdfs3/wire-encryption-fixes.patch` compiles. The DIGEST-MD5 sequence and MAC checks were tested with
a standalone harness. It has not run against a Kerberos cluster, because the environment where it was written had no
KDC.

- **Run:** `test/scripts/run_kerberos_tests.sh`, in every wire mode (none, integrity, privacy, aes, encrypt-key).
- **The `aes` mode matters most.** It covers the key-order fix, which failed every AES connection on GCC builds.
- **Consider adding tests:**
  - a DataNode that answers `ERROR_UNKNOWN_KEY` (e.g. restart the cluster with a client still connected);
  - an expired block token.

### 2. Run the cluster tests and code checks in CI

These items were deliberately left out because they change CI.

- **Cluster tests in CI.** Run `test/scripts/run_hdfs_tests.sh` in a CI job, as DuckDB's httpfs runs its S3 tests
  against MinIO. Run the Kerberos suite nightly. Today CI runs only `hdfs_no_cluster.test`.
- **clang-tidy in CI.** Bring it back in a job that installs the vcpkg dependencies. DuckDB uses it to enforce naming
  and smart-pointer rules. It passes locally today with DuckDB's `.clang-tidy`.
- **`make test_unit` in CI.** Run it after the build; it covers the crypto and batched-listing C++ tests.
- **Cost:** a few minutes per run for the Docker cluster, and one more vcpkg build for clang-tidy.

### 3. Keep the listing parallel when batched listing is on

A worker takes up to 100 waiting directories (the batch limit) and lists them in one call, paging through the
response. On wide trees, one worker can end up holding most of the pending directories while the others sit idle.
See `HdfsWalkImpl::Work` in [hdfs_client.cpp](../src/hdfs_client.cpp).

- **Fix:** cap each batch at roughly `pending / max_parallelism`.
- **Check:** benchmark a wide partitioned layout with [test/bench](../test/bench/README.md).

### 4. Ask DuckDB to treat hdfs:// as a remote file system

DuckDB decides whether a file is remote from a fixed list of URL prefixes (`EXTENSION_FILE_PREFIXES`, used by
`FileSystem::IsRemoteFile`). `hdfs://` isn't on that list, so DuckDB treats HDFS paths as local:

- **Caching.** The external file cache only stores HDFS reads with `SET cache_local_files = true`.
- **Cache tuning.** Local block sizes are used, and cached data never spills to disk.
- **Throughput.** Read throughput is not recorded.

The fix belongs in DuckDB: a `FileSystem` should be able to declare that it is remote, instead of DuckDB matching a
fixed prefix list. Under DuckDB's [AI policy](https://github.com/duckdb/duckdb/blob/main/AI_POLICY.md), a person has
to write and raise that issue or pull request. Once DuckDB has such a hook, implement it here and drop the README's
`cache_local_files` advice.

### 5. Stream glob results without losing their order

`HdfsGlobResult` collects the whole walk before DuckDB sees any file, so scans keep a fixed, sorted file order. To
start scanning before a large listing finishes, the walk would have to hand out entries in path order. That means
holding back a directory's results until every directory sorting before it is done. It needs a redesign of
`HdfsWalkImpl`'s scheduling, and a benchmark to show the gain.

### 6. Tests for two fixes that have none yet

- **Shared readers.** Opening a file that was overwritten, deleted or renamed through this client must not reuse the
  old reader (`HdfsClientImpl::ForgetReaders`, and the `expected_length` check). sqllogictest can't keep a handle open
  between statements, so this needs a C++ test or a `concurrentloop` test.
- **KMS key cache.** Keys must be scoped per connection and expire after `KeyCache::TTL`. A C++ test against a stub
  HTTP server would cover it.

### 7. FileSync

`HdfsFileSystem::FileSync` does nothing: data is durable once `HdfsWriter::Close` returns. If libhdfs3's
`OutputStream` exposes `sync()` (hflush/hsync) at the pinned commit, call it so `FileSync` means what DuckDB expects.

### 8. Reduce the libhdfs3 patch set

[docs/UPDATING.md](UPDATING.md) lists ten patches (about 3,700 lines). Either offer them to ClickHouse/libhdfs3, or
keep them as a fork with history and tests. Start with the small, general ones: `protobuf-uint8`,
`config-update-merges`, `rename2` and `batched-listing`.

### 9. Extend hdfs secrets

The `hdfs` secret supports `CONFIG_DIR` and `USER`. Possible additions:

- a Kerberos principal and keytab (keytab login isn't supported yet; see the README);
- KMS settings.

Each new parameter needs the connection key in `HdfsFileSystem::GetConnection` to include it.

### 10. Smaller follow-ups

- **`std::` types.** `HdfsWalkImpl` still uses `std::vector`, `std::deque` and `std::mutex` directly; switch them to
  DuckDB's types where that doesn't fight libhdfs3's.
- **Thread creation failure.** If starting a worker thread fails in `HdfsWalkImpl::SpawnIfNeeded`, the process
  terminates. Report it as a walk error instead.
- **Next DuckDB bump.** DuckDB is replacing many `ClientContext` uses with a new `Context` class (duckdb/duckdb#28314).
  Check the `FileOpener` and setting lookups when moving past v2.0.0.

## Decisions

These were taken deliberately. Revisit them only with a reason.

| Decision | Why |
|---|---|
| Glob results are sorted and handed over once the walk is complete (item 5). | File order, and so row order, stays fixed, as with local globs. The parallel walk finishes in no fixed order. |
| Globs use DuckDB's matcher (`duckdb::Glob`), so `?` matches one byte. | A pattern behaves the same on local files and on HDFS, and matching takes linear time. A two-byte UTF-8 character needs `??`. |
| A pattern that matches nothing is tried as a literal path. | DuckDB's local glob does the same; it makes a file named `data[1].csv` readable. |
| Hidden names (leading `_` or `.`) are skipped by default. | It's the Hadoop and Spark convention (`_SUCCESS`, `_temporary`). DuckDB's local glob doesn't skip them, so this is a documented difference. |
| Errors map to DuckDB exception types: invalid patterns are `Invalid Input Error`, bad configuration is `Invalid Configuration Error`, everything else is `IO Error`. | It matches how DuckDB classifies errors. |
| `hdfs_batched_listing` replaces the `duckdb.hdfs.batched-listing.enabled` key in `hdfs-site.xml`. The key still works, and batching happens only if both allow it. | DuckDB behaviour belongs in DuckDB settings, and existing configurations keep working. |
| `hdfs` secrets are the place for per-URL configuration; the Hadoop environment is the fallback. A `USER` on a Kerberos cluster is an error. | This follows DuckDB's `s3` and `azure` secrets. On Kerberos, the identity is the ticket's principal. |
| The KMS client stays on libcurl rather than DuckDB's `HTTPUtil`. | Core `HTTPUtil` can't do https or SPNEGO without the httpfs extension. |
| The directory walk keeps its own threads rather than using DuckDB's `TaskScheduler`. | The walk runs in the file-system layer, outside a pipeline. It can be cancelled through `ClientContext::BlockingInterruptCheck`. |
| libhdfs3 fixes go in a separate `wire-encryption-fixes.patch`. | Editing `wire-encryption.patch` in place would break the three patches that apply after it. |
| The crypto and batched-listing helpers keep C++ unit tests; glob behaviour is tested in SQL (`hdfs_glob.test`). | DuckDB prefers sqllogictest. The crypto tests check NIST test vectors, which SQL can't reach. |
