# Extension updating
When a new DuckDB is released, update the extension to target it as follows:

- Bump submodules
  - `./duckdb` should be set to the latest tagged release (e.g. `v2.0.0`). CI builds DuckDB from this pinned commit.
  - `./extension-ci-tools` should be set to the branch for that release line. Since v2.0 these branches are named
    `vX.Y-<codename>` (e.g. `v2.0-cyanoptera` for DuckDB v2.0.x); older lines used per-patch branches such as
    `v1.5.4`. Keep `branch =` in `.gitmodules` in sync.
- Bump `./.github/workflows/MainDistributionPipeline.yml`
  - Point both reusable workflows (`_extension_distribution.yml` and `_extension_code_quality.yml`) and their
    `ci_tools_version` inputs at the same `extension-ci-tools` branch.
  - Do **not** pass `duckdb_version`: with a `duckdb` submodule present, CI uses the commit it pins. That keeps local
    and CI builds on the same DuckDB.

# API changes
DuckDB extensions built with this extension template are built against the internal C++ API of DuckDB. This API is not guaranteed to be stable.
What this means for extension development is that when updating your extensions DuckDB target version using the above steps, you may run into the fact that your extension no longer builds properly.

Currently, DuckDB does not (yet) provide a specific change log for these API changes, but it is generally not too hard to figure out what has changed.

For figuring out how and why the C++ API changed, we recommend using the following resources:
- DuckDB's [Release Notes](https://github.com/duckdb/duckdb/releases)
- DuckDB's history of [Core extension patches](https://github.com/duckdb/duckdb/commits/main/.github/patches/extensions)
- The git history of the relevant C++ Header file of the API that has changed

## v1.5 → v2.0 notes for this extension
- Function, setting and column names are `Identifier`s. `Identifier` converts implicitly from string literals but
  only explicitly from `std::string`, and table-function binds return `vector<Identifier> &names`.
- Table-function named parameters are declared on the signature (`GetSignature().WithTypedKwargs(...)`) instead of
  the removed `named_parameters` map.
- `FlatVector::GetData<T>` returns `const T *`; writers use `FlatVector::GetDataMutable<T>`.
- A scalar function's info is set with `SetExtraFunctionInfo` and read with
  `BoundFunctionExpression::Function().GetExtraFunctionInfo()`.
- Scalar functions are non-fallible by default, and DuckDB now enforces it: any function that can throw must call
  `SetFallible()`.
- `COPY` creates and cleans up output directories through `CreateDirectoryExtended` / `RemoveDirectoryExtended`,
  which `HdfsFileSystem` overrides.
