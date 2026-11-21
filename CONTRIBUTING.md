# Contributing

## Build

Follow [Building](README.md#building) in the README: initialize the submodules, set up vcpkg, export
`VCPKG_TOOLCHAIN_PATH`, then run `make`. The first build compiles libhdfs3 and its dependencies through vcpkg and
takes a while; later builds are incremental.

## Test

Every change in behaviour needs a test. Prefer SQL tests (`test/sql/*.test`, DuckDB's
[sqllogictest](https://duckdb.org/dev/sqllogictest/intro.html)); use the C++ unit tests in `test/cpp/` only for what
SQL can't reach. [test/README.md](test/README.md) describes each test file.

```sh
make test                           # SQL tests that need no cluster
make test_unit                      # C++ unit tests (after make)
test/scripts/run_hdfs_tests.sh      # Docker HDFS cluster tests
test/scripts/run_kerberos_tests.sh  # Kerberos cluster, every wire-protection mode
```

The Integration Tests workflow (`.github/workflows/Integration.yml`) runs all four on Linux for every pull request
and every push to `main` and `duckdb-review`; changes that touch only docs skip it, and a new push to a pull request
cancels its previous run. For changes that add or remove NameNode round trips, also run `test/bench/run_bench.sh --check` and
include its numbers (see [test/bench/README.md](test/bench/README.md)).

Format C++ with the repository's `.clang-format` (`make format-fix`, or `clang-format -i` on the files you
changed), and Python with `black -l 120`.

## Commits and pull requests

- **Keep changes small.** One topic per pull request: a bug fix with its test, a new setting, a libhdfs3 patch. Don't
  mix renames or reformatting with behaviour changes; put them in a commit of their own.
- **Describe the change.** The commit title says what changed. The body says why, and how it was tested (which
  scripts ran, against which cluster).
- **libhdfs3 patches** go in `vcpkg_ports/libhdfs3/`. Follow [docs/UPDATING.md](docs/UPDATING.md): add the patch to
  the inventory table and bump `port-version`.
- **User-visible changes** (functions, settings, secret parameters, supported features) update the README in the
  same pull request.
- AI-assisted contributions follow DuckDB's [AI policy](https://github.com/duckdb/duckdb/blob/main/AI_POLICY.md):
  a person reviews, understands and answers for every change they submit.

## Reporting bugs

Use the bug report issue template. A reproduction against the Docker test cluster (`test/scripts/hdfs_up.sh`) is
the quickest to act on.
