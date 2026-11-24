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
| R8 | **`hdfs_ls`'s own literal fallback still does `Exists` and then a listing.** C7 fixed only the scan's fallback. That is two RPCs, with a race between them: a path deleted in between fails instead of returning no rows. | hawkfish, kryonix, dtenwolde, maxxen, mytherin (5) | **Fixed for files.** One stat; a file is its own row. A directory is still a stat and then its listing, so the race remains for directories (a NOT_FOUND from that listing would have to mean no rows, the C11 groundwork). Pinned in `hdfs_rpc_log.test` for a file, a directory and a miss. |
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
  - Test: `hdfs_rpc_log.test`. With `parquet_metadata_cache` on, a repeated `count(*)` over a glob of two files
    never opens the second one (no `getBlockLocations`); the control without the cache does; a replaced file is
    counted again. A single file doesn't need the etag: its cached footer is checked through the open handle's tag.
    (Round 3 first tested a single file, which passed without the etag; see the PR review loop.)
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
| An unclosed `{` blocks the literal fallback | kryonix, dtenwolde (clear: treat it as literal, or fall back on the parse error) | Now that patterns are checked before connecting, the fallback would have to come before that check. That is a choice of which invalid patterns become literal names. The same goes for a file literally named `{..}` or `{.}` (legal in HDFS): R1 refuses that pattern, and the file is reached as `\{..\}`. |
| Parquet opens a literal-fallback file twice (2 `getBlockLocations`) | lnkuiper, tmonster, dentiny, pdet, mytherin | Pinned (R11), not traced. The leading theory: `open_readers` holds weak pointers, so a reader is shared only while the bind-time handle is still open. The FileSystem log's OPEN/CLOSE order would settle it. |
| Pre-1970 mtimes | hawkfish | Round 2 described this wrongly: the listing path keeps negative times (the `uint64_t` round trip preserves the bits). The only clamp is `OpenFileExtended`'s `MaxValue(..., 0)`. Changing it needs a 1969 fixture to test, and it is unclear whether the NameNode accepts that. With the listing etag, such a file's etag (negative mtime) never matches its handle's tag (clamped): a Parquet metadata cache miss, never a stale read. |
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
- **The README says which platforms run tests** (carlopi). The first version was wrong (the PR review loop fixed it):
  CI builds linux_amd64, linux_arm64 and osx_arm64, and tests linux_amd64 and osx_arm64; osx_amd64 and musl are
  opt-in in extension-ci-tools and aren't built.
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
  ceilings. The SQL expectations written this round that only CI could confirm all passed there:
  - the pinned 2 `getBlockLocations`;
  - the stale control read;
  - the etag test (in its PR-review-loop form: no `getBlockLocations` for the second file of a cached glob).
    Its first form, zero `READ` entries on a cached single-file `count(*)`, was replaced.

## PR review loop

After the round, the quorum reviewed PR #5 itself (`git diff b98f47b HEAD`), each persona as its own read-only
subagent, in up to 5 iterations: fix what is raised, get CI green, review again, until no persona requests changes.

### Iteration 1 (on cd61dd0)

**0 request changes, 15 approve with nits, 0 approve.**

Raised, and fixed:

| Finding | Lenses | Fix |
|---|---|---|
| The etag test read a single file, whose cached footer is checked through the open handle's tag. It would have passed without the etag. (mytherin, maxxen, carlopi, pdet and dtenwolde judged that it could fail; hawkfish, tmonster, samansmink, lnkuiper, taniabogatsch and tishj traced the single-file path and found it couldn't.) | 11 lenses | The test now globs two files. On the repeated `count(*)` the second file is never opened (no `getBlockLocations`). The control without the cache shows it is opened, and a replaced file is counted again (kryonix, dtenwolde: the negative case). It moved to `hdfs_rpc_log.test`. The README says what the etag adds. |
| The pattern was parsed twice per glob, three times on a retry. Each parse may build up to 16 MiB, and two parse sites can drift apart. | 11 lenses | Parsed once, in `OpenGlobStream`. `HdfsClient::Glob` takes the `shared_ptr<const HdfsGlobPlan>`, and the stream keeps it for its retry. |
| The new pre-connect wrapper put the whole URL (up to 300 KB) back into the error. The dot error printed the whole pattern. | szarnyasg, pdet, dentiny, carlopi | `HdfsGlobPlan::Excerpt` (200 bytes and "...") in every pattern error and in the wrapper. Unit test, plus an exact-message no-cluster test. |
| The `CONFIG_DIR` check approved one string and handed libhdfs3 another: `~` and `file://` are expanded or stripped for the check only. It also ran without the opener. | hannes, maxxen, mytherin, tishj, kryonix, lnkuiper | With `enable_external_access = false`, `CONFIG_DIR` must be absolute. The check passes the opener. The message says what to do, and names `allowed_paths` too (szarnyasg, dtenwolde). Tests: `~/conf`, `file:///…` and `/data/../etc` are refused. |
| The `hdfs_ls` fallback's directory branch had no test, and R8 claimed the race fixed for directories too. | tmonster, dentiny, mytherin, tishj, maxxen, hannes, dtenwolde | A directory case is pinned (getFileInfo 1, listDirectory 2, children listed). R8 and the code comment now say the race remains for directories. A file's fallback row is checked against the listing's row (hawkfish). |
| The README's platform claim was wrong: osx_amd64 and musl aren't built at all. | carlopi | Corrected. |
| Pushes to `duckdb-review` shared one concurrency group, so a second merge cancelled the first one's run. | maxxen | `duckdb-review` keeps one run per commit, like `main`. |
| The test scripts leaked their temp file on a failure. The skip guard greps a core string that could change. | taniabogatsch, lnkuiper, samansmink | Both helpers clean up before exiting. `run_hdfs_tests.sh` first checks that an unmet `require-env` still produces the skip marker. |
| Doc wording: the rename fallback without file ids, the glob and budget sentences, the etag sentence, the CONTRIBUTING wrap, the pre-1970 etag miss, files named `{..}`, `CONFIG_DIR` as trusted configuration. | szarnyasg, taniabogatsch, maxxen, tishj, tmonster, mytherin, hawkfish, samansmink, dtenwolde, hannes | Fixed in the README, CONTRIBUTING, the comments at the clamp and in the fallback, and the open-items table above. |

Declined:

- **Restoring `PRAGMA enable_verification` in `hdfs_edge.test`** (taniabogatsch: the guideline bans it only in
  new tests, and the file was the only verified coverage). This is in dispute: round 3 removed it on the consensus
  of taniabogatsch, mytherin and pdet, and this iteration mytherin, samansmink, tishj, pdet and others checked the
  removal as right. Left removed, and listed for the owner below.
- **Also checking `CONFIG_DIR` at `CREATE SECRET`** (pdet). The use-time check is needed anyway, for persistent
  secrets created before external access was disabled, so a second site would only duplicate it.
- **Building osx_amd64 and musl in CI** (carlopi, `opt_in_archs`). A CI addition for its own PR.

### Iteration 2 (on 95d6262)

**0 request changes, 15 approve with nits, 0 approve.** The iteration-1 fixes were confirmed by every lens; what
was raised is smaller, mostly in the new code.

Raised, and fixed:

| Finding | Lenses | Fix |
|---|---|---|
| `Excerpt` cut at byte 200 and could split a multi-byte UTF-8 character, putting invalid UTF-8 into error text that clients decode strictly. | all 15 | The cut backs off to a character boundary. Unit tests with a 2-byte and a 3-byte character across the cut. |
| The excerpt didn't reach two places that quote the pattern: the literal `.`/`..` check (run before the glob parse) and the walk's own errors (`list '<pattern>' failed`). | tmonster, hawkfish, kryonix, maxxen | Both use the excerpt. A no-cluster test pins the literal `..` case. |
| Iteration 1 also cut plain listing paths at 200 bytes, which can hide which deep directory failed. | carlopi | Only glob patterns are excerpted; a listed path, bounded by the NameNode, is quoted whole. |
| `HdfsClient::Glob(pattern, plan, options)` took the pattern and its parse separately, so they could disagree; `plan` could be null. | samansmink, maxxen, mytherin, taniabogatsch | `Glob(plan, options)`: the plan keeps its pattern, and a null plan is asserted against. |
| The absolute-path rule read `enable_external_access` through the opener, `CanAccessFile` from the config. | hannes, maxxen, taniabogatsch, mytherin, dtenwolde | Both read the config's setting. |
| The `~/conf` case couldn't fail: `~` expanded outside the allowed directories anyway. | taniabogatsch, dtenwolde, carlopi, hawkfish, pdet | `home_directory` is set to the allowed `/data`, and the case is `~/hadoop`, so only the absolute-path rule can refuse it. |
| The replaced-file case changed the row count, and its comment credited the file id. | samansmink, lnkuiper, taniabogatsch, tishj, mytherin, dtenwolde, dentiny, pdet | The replacement has as many rows with new values; the test checks `sum(v)` and that the file is opened again. The comment says what changes, and points to the CLI check for the id-only case. |
| The bounded-error regex ended in `.*`, so a long tail would pass. | lnkuiper | The tail is bounded (`.{0,1000}`). |
| The fallback-row check was a one-way `EXCEPT`. The `listDirectory 2` pin wasn't explained. | tmonster, kryonix, lnkuiper, dtenwolde | Symmetric `EXCEPT ALL`; the comment names both listings. |
| `run_kerberos_tests.sh` hard-coded the skip marker and had no self-check. | samansmink, maxxen, lnkuiper, dentiny, mytherin, pdet | It has the same marker variable and self-check as `run_hdfs_tests.sh`. |
| README wording: the `CONFIG_DIR` paragraph order and wrap, the platform sentence, "without opening the files", the single-file aside, a long line; what an `allowed_paths` entry grants. The round-3 testing notes still named the removed single-file etag test. | szarnyasg, tishj, dentiny, dtenwolde, mytherin, hawkfish, carlopi | Rewritten. |

Declined:

- **Refusing `.`/`..` in `CONFIG_DIR`** (hannes: a symlink plus `..` is resolved physically by the kernel but
  lexically by the check). Core's `LocalFileSystem::CanonicalizePath` resolves existing paths with realpath
  (`TryCanonicalizeExistingPath`), so the check already sees the physical directory (pdet traced the same).
- **The etag test depends on file age** (tishj: without an etag the cache falls back to the mtime and its 10 s
  margin). It doesn't: `ExternalFileCache::IsValid` compares tags whenever either side has one, and the cached
  handle always has one, so a missing etag never matches, however old the file.
- **Pinning the full RPC budget of the cached and control runs** (tmonster). The exact counts (for the bind's file)
  aren't known without a run, and the discriminating pin, the second file, is in place.
- **Moving the new fixtures out of `/test`** (kryonix: local re-runs of `hdfs.test` on the same cluster see extra
  files). The existing `rpc[1].parquet` fixture already does this; CI starts from a clean cluster. A follow-up for
  all of them.
- **A docs link in the `CONFIG_DIR` error** (szarnyasg). There is no stable anchor yet.

### Outcome

The loop stopped after iteration 2, because neither iteration had a request for changes. CI was green on the head
of each one (cd61dd0 before the loop, 95d6262 after iteration 1, 23f8f2d after iteration 2), including the cluster,
Kerberos and benchmark jobs. The quorum has not re-reviewed iteration 2's fixes; they were all nits, each with a test.

### Still in dispute

- `PRAGMA enable_verification` in `hdfs_edge.test`: taniabogatsch would restore it; round 3's consensus removed it.
  The owner decides.
