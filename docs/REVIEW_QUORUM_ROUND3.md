# DuckDB review quorum, round 3

The third pass of the DuckDB review quorum over the `duckdb-review` branch, at b98f47b (round 2, PR #4, merged on
top of round 1's d00cbc7 and the original 6455336). The 15 reviewer personas and the process are in
luke-falvey/duckdb-webhdfs, `.github/agents/`. They are lenses built from public DuckDB activity, not the people:
nothing below is what a real DuckDB contributor said.

## Scope

All of `src/`, `test/`, the CI workflows, the test scripts, `vcpkg_ports/` and the docs at b98f47b. Each persona ran
as its own read-only subagent with the same brief:

- what rounds 1 and 2 changed (`git diff 6455336 d00cbc7`, `git diff d00cbc7 b98f47b`);
- what [round 2](REVIEW_QUORUM_ROUND2.md) fixed and left open.

Each was asked four things, with file:line, severity and a suggested fix:

- what the earlier rounds missed;
- what their fixes broke or left half done;
- new problems;
- for each item round 2 left open, whether its right answer is now clear.

No persona saw another's answer. Severities are the reviewers' own. Line numbers refer to b98f47b.

## Verdict tally

| Persona | Verdict | Main concern |
|---|---|---|
| mytherin | Approve with nits | `..` through braces or escapes; `hdfs_ls` fallback makes two calls; listing retry logs to the wrong query |
| hannes | Request changes | `..` through braces or escapes (high); brace expansion memory; secret `CONFIG_DIR` |
| lnkuiper | Approve with nits | Brace expansion memory; inverted CI concurrency group; listing retry logger |
| carlopi | Approve with nits | Inverted concurrency group; the cache check has no positive control; skipped tests count as run |
| tishj | Request changes | `..` through braces or escapes; brace expansion memory; skipped tests count as run |
| taniabogatsch | Request changes | `..` through braces or escapes; the README promises a log entry that logging-off never writes |
| hawkfish | Approve with nits | The pre-1970 clamp is elsewhere than round 2 said; no value test of any time column; `0:` tags |
| samansmink | Request changes | `..` through braces or escapes; concurrency group; the C3 test passes without the fix |
| maxxen | Approve with nits | `..` through braces or escapes; brace expansion memory; `hdfs_ls` fallback |
| pdet | Approve with nits | `..` through braces or escapes; invalid patterns found only after connecting; concurrency group |
| tmonster | Approve with nits | The fallback pin filters out the op that regressed; no check can show a cache hit |
| dtenwolde | Request changes | `..` through braces or escapes; brace expansion copying; `hdfs_ls` fallback |
| szarnyasg | Approve with nits | Docs and a setting description made stale by round 2; the cache check can pass without testing |
| kryonix | Request changes | `..` through braces or escapes; brace expansion memory; no proof that LIMIT stops a walk |
| dentiny | Approve with nits | Brace expansion memory; the cache check can pass without testing; rename recovery by length |

**6 request changes, 9 approve with nits, 0 approve.** Round 2 had 12 request changes and 3 approve with nits.

## Consensus

These items were raised by 3 or more lenses, or by 2 with high severity that traced the same cause.

| # | Finding | Lenses | Status |
|---|---|---|---|
| R1 | **Round 2's `.`/`..` refusal (C13) is checked on the raw URL.** Brace expansion and unescaping happen later, in `HdfsGlobPlan::Parse`. So `hdfs://nn/allowed/{..}/secret/*`, `\.\.`, `{.}{.}` and `{..,x}` pass the check, and the walk root becomes `/allowed/../secret`. DuckDB's `allowed_directories` prefix check passes too. Opening a listed file is still refused, but `hdfs_ls`, `glob()` and a metadata-only `read_blob` return names, sizes and owners. Whether the client or NameNode resolves `..` couldn't be checked here, but either way "refused before connecting" was false for globs. | hannes (high), kryonix, samansmink, taniabogatsch, dtenwolde, tishj, maxxen, mytherin, pdet (9) | **Fixed.** `Parse` refuses a literal `.` or `..` component after expansion and unescaping. `OpenGlobStream` parses the pattern before it connects. Tests: no-cluster cases in `hdfs_access.test` (`hdfs_ls`, `glob`, `read_blob`), and a `test_glob` unit case. |
| R2 | **The brace caps bound depth and count, not bytes.** A 60 KB pattern with 13 `{a,b}` groups passes every cap. It builds 8,192 copies of 60 KB (about 0.5 GB), and `Parse` copies them again, all outside `memory_limit`. Groups with one alternative make each leaf copy the whole pattern once per group. | dentiny, lnkuiper, kryonix, hannes, taniabogatsch, dtenwolde, tishj, maxxen (8) | **Fixed.** Expansion has a 16 MiB budget that counts every intermediate pattern, and throws as soon as it is spent. Tests: unit cases (both shapes, and two large legitimate patterns that must still expand), plus a no-cluster SQL case. |
| R3 | **The version-tag CLI check (C2) can pass without exercising the fix.** `.shell` output went to `/dev/null`, and the CLI ran without `-bail`. Nothing asserted three preconditions: equal sizes, an equal mtime after the replace, and that the cache was filled. Any of those failing prints "1 2" even with the old tag. | szarnyasg, dentiny, lnkuiper, hannes, carlopi, taniabogatsch, tmonster, dtenwolde, tishj, pdet (10) | **Fixed.** The check now runs with `-bail` and reports touch or copy failures. It asserts equal sizes, the exact mtime (2020-01-01 UTC, so it also tests a time value: hawkfish) before and after, and a filled cache. A control read with `validate_external_file_cache = 'NO_VALIDATION'` must then see the stale value. |
| R4 | **The "every cluster test ran" guard counts a file as run even when unittest skipped it.** An unmet `require parquet` or `require-env HDFS_TEST_NO_KMS` skips the file and still exits 0. The Kerberos files had no guard at all. | szarnyasg, samansmink, carlopi, taniabogatsch, dtenwolde, tishj, maxxen, pdet, lnkuiper (9) | **Fixed.** Both scripts fail when unittest reports a skip. `run_kerberos_tests.sh` also checks that every `require-env HDFS_KERBEROS*` file ran. |
| R5 | **The two version tags disagree when the file id is unknown.** `GetVersionTag` returns `0:mtime:len`, which the caches trust without a margin. `GetStatsIfExists` returns no tag. | hawkfish, lnkuiper, carlopi, taniabogatsch, tmonster, maxxen, mytherin, pdet (8) | **Fixed.** `HdfsVersionTag` is the one rule: no tag without a file id. The caches then fall back to the mtime and its margin. |
| R6 | The `GetVersionTag` comment still says `<mtime>:<length>`. | 8 lenses | **Fixed.** |
| R7 | **The concurrency group is inverted.** It appends the sha on every ref except `main`. Pushes to a PR never cancel the run they supersede, so multi-hour Integration runs stack up, while runs on `main` cancel each other. Round 2's C8 removed only the double trigger. | lnkuiper, samansmink, carlopi, maxxen, mytherin, pdet (6; taniabogatsch noted it) | **Fixed.** Both workflows use extension-ci-tools' form, `github.ref != 'refs/heads/main' \|\| github.sha`. |
| R8 | **`hdfs_ls`'s own literal fallback still does `Exists` and then a listing.** C7 fixed only the scan's fallback. That is two RPCs, with a race between them: a path deleted in between fails instead of returning no rows. | hawkfish, kryonix, dtenwolde, maxxen, mytherin (5) | **Fixed.** One stat. A file is its own row, and only a directory is listed. Pinned in `hdfs_rpc_log.test` for a match and for a miss. |
| R9 | **The rest of C3: the listing retry logs to the wrong query.** A listing retried after a connection error starts its walk on whichever thread calls `Next()`. The walk then logs to that thread's last query. | lnkuiper, tishj, mytherin (3) | **Fixed.** The stream keeps the opening query's logger and scopes the retry to it. **No test:** it needs a connection failure in the middle of a listing, i.e. the fault-injection harness (PLAN item 7). |
| R10 | **Invalid patterns are found only after connecting.** An unclosed `{`, two `**` or the caps above surface as a connect timeout behind an unreachable NameNode. | pdet, tishj, maxxen, dtenwolde (4) | **Fixed** with R1. The cap cases now also run without a cluster (`hdfs_no_cluster.test`). |
| R11 | **The literal-fallback RPC pin filters out `getBlockLocations`,** the one op round 2's CI found doubled. | lnkuiper, tmonster (2, medium); pdet and mytherin want a pin | **Pinned** at 2, with a note. Tracing the cause is still open (below). |
| R12 | **The C3 test passes on d00cbc7 too:** the create and the abort run in one query on one connection. `count(DISTINCT query_id)` ignores NULL, so an entry logged without a query isn't caught. | samansmink, pdet (2) | **Half done.** Asserts that no entry has a NULL `query_id`. A test that tells the old code from the new needs two connections on one thread (C++, against a client stub), so it is open. |

## Open items from round 2, revisited

Implemented, because three or more lenses called the answer clear:

- **A secret's `CONFIG_DIR` under `enable_external_access = false`** (hannes, samansmink, maxxen, tishj). The
  directory must pass `DBConfig::CanAccessFile` as a directory, as any local read would. Otherwise the connection is
  refused with a Permission Error naming the secret. `HADOOP_CONF_DIR` and `HADOOP_HOME` come from the operator's
  environment and are not checked; the README says so. Tests: `hdfs_access.test`, with an allowed and a refused
  directory.
  - szarnyasg's related finding, that a secret `CONFIG_DIR` without XML files is ignored silently, doesn't hold. It
    is already an error (`hdfs_client.cpp:1303`, tested in `hdfs_no_cluster.test`).
  - Only a `HADOOP_CONF_DIR` pointing at a missing directory is still silent. That stays open, as an environment
    compatibility question.
- **Listings carry an etag** (lnkuiper, tmonster, pdet; maxxen wanted a test that shows a hit).
  - Why: core's `ParquetFileMetadataCache::IsValid(OpenFileInfo)` compares the cached handle's version tag with the
    listing's `etag`. With a tag on one side only, it never matched, so cached row-group statistics were never
    trusted on HDFS.
  - Change: the listing now emits the same `HdfsVersionTag`.
  - Test: `hdfs.test`. With `parquet_metadata_cache` on, a repeated `count(*)` reads no bytes (FileSystem log).
- **`PRAGMA enable_verification` removed from `hdfs_edge.test`** (taniabogatsch, mytherin, pdet). DuckDB's test
  guidelines no longer allow it.

Still open:

| Item | Lenses | Why it is open |
|---|---|---|
| C11, the `ListFilesExtended` contract | dentiny, tishj, taniabogatsch, mytherin (implement); kryonix, maxxen, pdet (keep) | The contract is now settled from core's source. mytherin said an empty directory returns false; `LocalFileSystem` returns true for it, and false for a missing path or a file (`opendir` fails). Matching it without an extra RPC needs the walk to report not-found and is-a-file distinctly (`HdfsListStream::Next` folds NOT_FOUND into an IO error). It also needs a C++ contract test. |
| `TryRemoveFile` / `RemoveFiles` (two RPCs per file on `COPY ... OVERWRITE`) | dentiny, tmonster, maxxen, pdet (one delete); mytherin (won't fix); taniabogatsch (keep) | A non-recursive HDFS delete also removes an empty directory. POSIX `remove()` does the same, but mytherin points out that `RemoveFile` already has this hazard. Decide the directory semantics first. |
| The connection cache is keyed by secret name | lnkuiper, maxxen (key on authority + `config_dir` + `user`); dtenwolde (agrees on the direction); samansmink (doubts connection-scoped secrets can collide) | The premise is disputed, and the change alters `hdfs_reset_connections()` counts. |
| `HdfsSecret` overriding `MatchScore` (and trailing-`/` scopes) | samansmink, maxxen (implement); tishj, dtenwolde (needs a decision) | A design change to secret matching. It depends on `BaseSecret::MatchScore` in v2.0 and on persistent-secret compatibility. |
| C6, the abort-failure lease | All who mentioned it: keep open | Two fixes are proposed: invalidate the client and retry the delete (carlopi), or write under a hidden temporary name and rename on close (taniabogatsch). Either needs the fault-injection harness (PLAN item 7). |
| Load the shipped `.duckdb_extension` in CI | carlopi, samansmink | A CI addition: a stock v2.0.0 CLI with `-unsigned`, and a `readelf` check for runtime dependencies. Its own PR. |
| An unclosed `{` blocks the literal fallback | kryonix, dtenwolde (clear: treat it as literal, or fall back on the parse error) | Now that patterns are checked before connecting, the fallback would have to come before that check. That is a choice of which invalid patterns become literal names. |
| Parquet opens a literal-fallback file twice (2 `getBlockLocations`) | lnkuiper, tmonster, dentiny, pdet, mytherin | Pinned (R11), not traced. The leading theory: `open_readers` holds weak pointers, so a reader is shared only while the bind-time handle is still open. The FileSystem log's OPEN/CLOSE order would settle it. |
| Pre-1970 mtimes | hawkfish | Round 2 described this wrongly: the listing path keeps negative times (the `uint64_t` round trip preserves the bits). The only clamp is `OpenFileExtended`'s `MaxValue(..., 0)`. Changing it needs a 1969 fixture to test, and it is unclear whether the NameNode accepts that. |
| `HdfsConnection::Get()` holds its lock across `Connect` | dentiny (shared future) | Needs a latency-injecting test. |
| Smaller test gaps | kryonix, tmonster, mytherin | Four gaps: no test shows that `LIMIT 1` stops a walk early; no hive-partitioning, `filename` or `union_by_name` combinations on HDFS globs; `bench.py --compare` drops baseline queries that didn't run; no TSan job for `make test_unit`. |
| Smaller design nits | lnkuiper, dentiny, mytherin | Encrypted writes copy the whole buffer. `GetStatsIfExists` stats directories that the listing already described. `OpenFileExtended` and `GetStatsIfExists` parse `extended_info` twice. Pass the RPC logger explicitly instead of through a thread-local. |
| HADOOP_CONF_DIR pointing at a missing directory | szarnyasg | Environment compatibility (see above); a warning in `duckdb_logs` is the cheap step. |

## Disagreements

- **Per-pattern cap or byte budget (R2).** hannes, maxxen, tishj and kryonix would cap each expanded pattern at the
  NameNode's 8,000-character path limit. dentiny, lnkuiper and taniabogatsch would cap the total. The budget was
  chosen. A 64 KiB pattern of many alternatives (a list of file names) is legitimate input, and the budget also
  bounds the intermediate copies that dtenwolde's one-alternative case makes.
- **The C11 contract.** mytherin wanted an empty directory to return false; tishj and dentiny wanted true. Core's
  `LocalFileSystem` returns true, so that is the target when C11 is done.
- **`TryRemoveFile`, the cache key, and `MatchScore`.** See the open table: each has lenses on both sides.
- **`hdfs_exists` and `CONSISTENT_WITHIN_QUERY`.** Settled: kryonix withdrew the round-2 objection.
- **Process.** szarnyasg again asks for one PR per topic with a human-written body. szarnyasg also asks that C5 (the
  metadata functions accept `hdfs://` URLs only) be called out as breaking before anything goes upstream. This round
  is one PR of a few topic commits, as asked. Splitting it and rewriting the descriptions is the owner's call.

## Unique flags implemented

Each was cheap and clearly right:

- **Rename recovery** (dentiny). It matched the target by length, so an old target of the same size (re-running a
  COPY) passed as the renamed source. It now compares file ids, since a rename keeps the inode; without ids it
  falls back to length. **No test:** the recovery runs only after a connection failure during the rename, so it
  waits on the fault-injection harness, like R9.
- **Docs made stale by round 2** (szarnyasg):
  - the `hdfs_list_parallelism` description used a scheme-less `hdfs_ls('/path/**')`, which is now an error;
  - CONTRIBUTING said Integration runs on every push;
  - PLAN gave a stale reason for the glob unit tests;
  - a source comment cited this review log instead of PLAN.
- **Logging claims** (taniabogatsch).
  - A dropped writer's failed commit is logged only when logging is on, and the README now says so.
  - The log message no longer claims the file was removed when the abort also failed.
  - The README notes that `mode` leaves out HDFS's ACL, encryption and erasure-coding flag bits.
- **The README says which platforms run tests** (carlopi). linux_arm64 and osx_amd64 are only built.
- **`ClassEnd`'s comment** (pdet). The comment and the unit test called a run of unclosed `[` linear. It is one scan
  per `[`, bounded by the 64 KiB cap.

## Recommendation

Merge R1–R11, the half of R12 that is done, and the three implemented open items.

- R1 and the `CONFIG_DIR` check close the two remaining ways round 2's access rules could be bypassed.
- R2 closes the last unbounded allocation from a single SQL literal.
- R3, R4 and R11 make the round-2 tests able to fail.

The largest concerns left are unchanged from round 2:

- C6 (the abort-failure lease), with the retry-logger test from R9, waits on the fault-injection harness.
- The secret design questions (`MatchScore`, the cache key) need an owner decision.

## Testing in this round

- **Locally:**
  - built DuckDB's static library from the pinned submodule;
  - `make test_glob`, `test_functions` and `test_batch` pass (the glob suite includes the new byte-budget and
    dot-component cases);
  - type-checked `hdfs_filesystem.cpp`, `hdfs_functions.cpp`, `hdfs_glob.cpp` and `hdfs_extension.cpp` against the
    DuckDB headers;
  - `bash -n` on both test scripts;
  - formatted with clang-format 11, the version CI uses.
- **Not run locally:** the full extension build. As in round 2, vcpkg's download of a dependency's GitHub archive
  (openssl) was refused with 403. CI runs the build, `make test`, the Docker cluster tests (including the new
  `hdfs_rpc_log.test` pins, the etag test and the rewritten cache check), the Kerberos suite and the benchmark
  ceilings. The SQL expectations written this round that only CI can confirm:
  - the pinned 2 `getBlockLocations`;
  - zero `READ` entries on the cached `count(*)`;
  - the stale control read.
