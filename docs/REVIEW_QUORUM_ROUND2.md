# DuckDB review quorum, round 2

The second pass of the DuckDB review quorum over the `duckdb-review` branch, after round 1 (PR #3, merged as
d00cbc7 on top of 6455336). The 15 reviewer personas and the process are in luke-falvey/duckdb-webhdfs,
`.github/agents/`. They are lenses built from public DuckDB activity, not the people: nothing below is what a real
DuckDB contributor said.

## Scope

All of `src/`, `test/`, the CI workflows, the test scripts and the docs at d00cbc7. Each persona ran as its own
read-only subagent with the same brief:

- what round 1 changed (`git diff 6455336 d00cbc7`);
- what round 1 deferred (splitting 6455336, which must not be rewritten; fault-injection tests; secret resolution
  for globs that cross scopes; KMS logging; KMS through `HTTPUtil`).

Each was asked for what round 1 missed, broke or left half done, and for new problems, with file:line, severity and
a suggested fix. No persona saw another's answer.

Severities below are the reviewers' own. Line numbers refer to d00cbc7.

## Verdict tally

| Persona | Verdict | Main concern |
|---|---|---|
| mytherin | Request changes | `hdfs_rpc_log.test` never runs; no TSan job; glob matching under the walk lock |
| hannes | Request changes | Brace expansion recursion can crash the process; secret `config_dir` bypasses external-access checks |
| lnkuiper | Request changes | Version tag lacks the file id; connection cache key collides for connection-scoped secrets |
| carlopi | Request changes | `hdfs_rpc_log.test` never runs; the destructor commit swallows failures; the shipped binary is never loaded in a test |
| tishj | Request changes | `hdfs_rpc_log.test` never runs; `..` and scheme-less paths pass the access check |
| taniabogatsch | Request changes | Log storage on HDFS breaks after a restart; silent destructor commit; abort-failure lease |
| hawkfish | Request changes | Version tag lacks the file id; pre-1970 mtimes clamped; no value tests for times |
| samansmink | Request changes | `hdfs_rpc_log.test` never runs; Integration runs twice per push |
| maxxen | Request changes | Listing URLs take another query's spelling of the host; core `FileSystem` hooks left at defaults |
| pdet | Approve with nits | `hdfs_rpc_log.test` never runs; scheme-less paths |
| tmonster | Request changes | `hdfs_rpc_log.test` never runs; the cache benchmark can't fail |
| dtenwolde | Request changes | Brace recursion; scheme-less paths; listing URLs take another spelling of the host |
| szarnyasg | Approve with nits | `hdfs_rpc_log.test` never runs; PLAN and test README stale |
| kryonix | Request changes | Brace recursion; scheme-less paths; no proof that LIMIT stops the walk early |
| dentiny | Approve with nits | Metadata-only reads open every file; connect holds the connection lock |

**12 request changes, 3 approve with nits, 0 approve.**

## Consensus

Items raised by 3 or more lenses, or by 2 with high severity that traced the same cause.

| # | Finding | Lenses | Status |
|---|---|---|---|
| C1 | `test/sql/hdfs_rpc_log.test` (added in round 1, cited by the README as pinning RPC counts) never runs. It needs `HDFS_TEST_RUNNING`, so `make test` skips it, and `run_hdfs_tests.sh` doesn't list it. | mytherin, samansmink, szarnyasg, tmonster, carlopi, pdet, taniabogatsch, tishj (8) | **Fixed.** The script runs it, and fails if any `require-env HDFS_TEST_RUNNING` file isn't run. |
| C2 | `GetVersionTag` is `mtime:length`, without the file id that round 1 added to reader sharing. DuckDB's external file cache trusts a tag alone, so a same-size replacement that keeps its mtime (`cp -p`, `distcp -pt`) is served stale. | tmonster, hawkfish, pdet, dentiny, lnkuiper, maxxen (6) | **Fixed.** The tag is `file_id:mtime:length`. A CLI check in `run_hdfs_tests.sh` replaces a file between two cached reads. |
| C3 | The RPC logger is a thread-local set only in `GetConnection`. Calls on an open handle (the abort's `delete`, a writer's stat) log to whatever query the thread last served, or not at all. | samansmink, hannes, taniabogatsch, tishj, lnkuiper, mytherin, maxxen (7) | **Fixed.** A handle keeps the opening query's logger and sets it around Close, Abort, the destructor and the stat. Test: a failed COPY's create and delete are logged under its query. |
| C4 | Brace expansion recurses once per group. The 10,000 cap counts only results, so `repeat('{a}', 100000)` overflows the stack and crashes the process; nested groups do the same. | hannes (high), dtenwolde, kryonix (3) | **Fixed.** At most 128 brace groups and 64 KiB per pattern. Unit tests, plus SQL tests on the cluster. A run of unclosed `[` is no longer quadratic (hannes). |
| C5 | `hdfs_ls`/`hdfs_stat`/`hdfs_exists` treat any non-`hdfs://` string as a path on fs.defaultFS, but the access check judges it as a local path. `allowed_directories=['/tmp/']` then lets `hdfs_ls('/tmp')` list HDFS. `''`, `s3://…` and typos also go to HDFS. | kryonix, pdet, tishj, dtenwolde, mytherin (5) | **Fixed.** The functions accept `hdfs://` URLs only, and the error points to `hdfs:///path`. |
| C6 | When the abort's delete fails, the output stream is deliberately leaked. The cached client keeps renewing the lease, so the path stays "being created" until `hdfs_reset_connections()` or process exit, and the code comments contradict the README. | carlopi, hannes, taniabogatsch, tishj (4) | **Docs fixed, behaviour open.** The comments and README now say what happens. The fix (invalidate or retire the client, then retry the delete) needs the fault-injection harness of PLAN item 7 to test, so it is left open. |
| C7 | The glob literal fallback calls `Exists` then `Stat`: two `getFileInfo` for one path, with a race between them. | tmonster, dentiny, tishj, lnkuiper (4) | **Fixed.** One stat. `hdfs_rpc_log.test` pins it. |
| C8 | The Integration workflow triggers on every push and on `pull_request`, in different concurrency groups, so each push to a PR branch runs the multi-hour job twice. | samansmink, tmonster, carlopi (3) | **Fixed.** Pushes trigger it only on `main` and `duckdb-review`; docs-only changes skip it. |
| C9 | `GetStatsIfExists`, `GetFileType` and `TryRemoveFile` stay at the core defaults. `hdfs://` counts as local, so metadata-only `read_blob`/`read_text` open every file (block locations, plus a KMS call in an encryption zone, which fails without a key provider). | dentiny, mytherin, maxxen (3) | **Fixed for `GetStatsIfExists`/`GetFileType`.** Answered from the listing, or one stat. Tests in `hdfs_rpc_log.test` and `hdfs_ez_no_provider.test`. `TryRemoveFile`/`RemoveFiles` (2 RPCs per file on COPY OVERWRITE) are open. |
| C10 | The `permissions` column drops the sticky bit (`rwxrwxrwx` for a 1777 `/tmp`), while `mode` may carry it. | szarnyasg, hawkfish, maxxen (3) | **Fixed.** Rendered as `t`/`T`, and `mode` is masked to 01777. Unit test, plus a 1777 cluster fixture. |
| C11 | `ListFilesExtended` departs from `LocalFileSystem`: an empty directory returns false, a missing one throws, and a file lists itself. | dentiny, tishj, mytherin (3) | **Open.** No current core caller reaches the differing cases (they check `DirectoryExists` first, and the COPY caller ignores the return value), so no SQL test could show a change. Worth doing with a C++ test of the contract. |
| C12 | Listing URLs use the shared connection's authority, i.e. the host spelling of whichever query connected first (a round-1 regression from the case-folded cache key). Secret scopes and allowed paths then see another spelling. | dtenwolde, maxxen (2, regression, same trace) | **Fixed.** Uses the caller's authority. Test in `hdfs.test`. |
| C13 | `..` components pass DuckDB's `allowed_directories` check, because remote paths aren't canonicalized, and are sent to libhdfs3 as is. | hannes, tishj (2, security, same trace) | **Fixed.** Paths with `.` or `..` components are refused for every operation, before connecting (HDFS forbids them in names). No-cluster tests. |
| C14 | A commit that fails while a handle is dropped without `Close()` (EXPORT DATABASE's files) deletes the file and swallows the error. Nothing tests EXPORT/IMPORT on HDFS. | carlopi, taniabogatsch (2, same trace) | **Fixed.** The error is logged. EXPORT/IMPORT round trip in `hdfs.test`. The logged entry itself needs fault injection to test (PLAN item 7). |
| C15 | Docs out of date: `docs/PLAN.md` still says CI has no cluster or Kerberos runs; `test/README.md` misses round 1's test files; the README says "one entry per NameNode RPC". | szarnyasg, tishj, samansmink (3) | **Fixed.** |

## Disagreements

- **`PRAGMA enable_verification` in `hdfs_edge.test`.** mytherin wants it removed (DuckDB's test policy forbids it in
  new tests, and the file is I/O-bound). pdet would extend verification (more buffer sizes). Left as is. This
  repository's own `duckdb-engineer` agent sides with removal, so it is a cheap follow-up if the owner agrees.
- **Walk threads outside DuckDB's scheduler.** kryonix and hawkfish want the walk's threads bounded by `threads` or
  per database. `docs/PLAN.md` records keeping the walk's own threads as a decision. The README now says the
  threads are outside the `threads` limit and per query. The cap stays at 64.
- **`hdfs_exists` and `CONSISTENT_WITHIN_QUERY`.** kryonix calls the declaration untrue without per-query
  memoisation. tishj, dtenwolde, mytherin, pdet and taniabogatsch checked it and found it correct. Only the wrong
  comment ("as for now()") was changed.
- **Secret scope matching.** maxxen wants the component-boundary loop replaced by an `HdfsSecret` that overrides
  `MatchScore`, so that `which_secret` agrees with the extension. samansmink, tishj and dtenwolde verified the loop.
  This is a design change, so it is open.
- **Reconnect policy.** lnkuiper wants timeouts and `RetriableException` (safe mode) not to tear down the client.
  Round 1 retries every CONNECTION error once. Open; it needs a classification decision and fault tests.
- **Process.** szarnyasg asks for one PR per topic with a human-written body (CONTRIBUTING and DuckDB's AI policy).
  This round is one PR of small commits, as asked. Splitting it, and rewriting its description in the owner's words
  before anything goes upstream, is the owner's call.

## Unique flags

Implemented, because they were cheap and clearly right:

- The pattern length cap also removes the quadratic scan over unclosed `[` (hannes).
- README corrections:
  - durability is "replicated", not fsynced, unless `dfs.datanode.synconclose` is set (taniabogatsch);
  - DuckDB's file log storage can't reopen a log file on HDFS, because appending isn't supported (taniabogatsch);
  - `VALIDATE_REMOTE` doesn't validate `hdfs://` files (tmonster).

Open, for the owner to weigh:

| Finding | Lens | Why it is open |
|---|---|---|
| A secret's `CONFIG_DIR` (and `USER`) is read outside DuckDB's external-access checks. The local XML can redirect NameNodes, the KMS URL and the ticket cache. | hannes (medium) | It needs a policy decision: refuse `CONFIG_DIR` under `enable_external_access=false`, check it against `allowed_directories`, or read the XML through DuckDB's FileSystem. |
| The connection cache is keyed by secret name. Connection-scoped secrets with the same name in two connections replace each other's client on every access. Entries are never evicted. | lnkuiper, dtenwolde, maxxen | Changing the key to the config identity changes `hdfs_reset_connections()` counts and connection sharing; it needs a design. |
| Files under construction read short (only complete blocks). | lnkuiper | Needs libhdfs3's visible-length handling verified, and a Java writer fixture. |
| `HdfsConnection::Get()` holds its lock across `Connect`, so N threads behind a blackholed NameNode wait N timeouts. | dentiny | Needs a shared-attempt design and a latency-injecting test. |
| Glob matching runs under the walk mutex. | hannes, mytherin | Performance only; needs a benchmark. |
| Block locations past `dfs.prefetchsize` are fetched again by every pooled stream. | dentiny | libhdfs3 internals, and an RPC counter for unlogged calls. |
| Listings carry no `etag`, so the parquet metadata cache never hits on HDFS. | maxxen | The etag would equal the version tag. Needs a test that observes cache hits. |
| An unclosed `{` blocks the literal fallback, so a file COPY wrote as `lit/{x` can't be read back by that name. | dtenwolde | Needs a decision on which invalid patterns fall back. |
| Pre-1970 mtimes are clamped to the epoch on the listing path (`HdfsFileStatus::mtime` is unsigned). | hawkfish | Type change across the client interface. |
| `HADOOP_CONF_DIR` pointing at a missing directory is silently ignored. | szarnyasg | Behaviour change for environments that set it loosely; needs a CLI test. |
| A secret scope with a trailing `/` doesn't cover the directory itself. | samansmink | Scope semantics; goes with the `MatchScore` question above. |
| No TSan job; no test loads the shipped `.duckdb_extension`; no https/SPNEGO KMS test; no hive-partitioning scans of HDFS globs; the cache benchmark can't fail; `bench.py --compare` drops missing queries silently. | mytherin, carlopi, tmonster, lnkuiper | CI and test additions, each its own PR. |
| Smaller nits: `rowid` branch dead code; `hdfs_stat` has no cardinality; walk options built twice; `require parquet` placement skips CSV-only tests; a Router's one `UNSUPPORTED` disables batching until reconnect; deprecated hdfs-site key has no warning; encrypted writes copy the whole buffer. | kryonix, hawkfish, dtenwolde, mytherin, pdet, taniabogatsch, lnkuiper | Low severity. |

## Recommendation

Merge the fixes above: C1–C5, C7–C10 and C12–C15, plus the docs half of C6.

They close the crash (C4), the access-check holes (C5, C13) and the silent staleness (C2). They also make round 1's
RPC-count guard actually run (C1) and log the RPCs round 1's log type misattributed (C3).

The largest concern left is C6, the abort-failure lease. Until fault injection exists, it is documented rather than
fixed. Of the unique flags, hannes's `CONFIG_DIR` side door is the one to decide next.

## Testing in this round

- **Locally:**
  - built DuckDB's static library;
  - ran `make test_glob`, `test_functions` and `test_batch`, all passing;
  - type-checked `hdfs_filesystem.cpp`, `hdfs_functions.cpp` and `hdfs_extension.cpp` against the DuckDB headers;
  - formatted with clang-format 11, the version CI uses.
- **Not run locally:** the full extension build. vcpkg could not download libhdfs3's dependencies here (GitHub
  archive downloads were refused, and DuckDB's binary cache was unreachable). CI runs the build, `make test`, the
  Docker cluster tests (including `hdfs_rpc_log.test`, now run for the first time), the Kerberos suite and the
  benchmark ceilings.
