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
- Bump `./.github/workflows/Integration.yml`
  - Set `VCPKG_COMMIT` to the `vcpkg_commit` default of the new `extension-ci-tools` branch
    (`.github/workflows/_extension_distribution.yml`), so the integration build uses the same vcpkg ports as the
    distribution build.

# Updating libhdfs3
The HDFS client is built by the vcpkg overlay port in `vcpkg_ports/libhdfs3/`, pinned to a ClickHouse/libhdfs3 commit.
To move to a newer commit:

- Update `REF` and `SHA512` in `vcpkg_ports/libhdfs3/portfile.cmake` (`curl -L <archive url> | shasum -a 512`) and
  `version-date` in `vcpkg_ports/libhdfs3/vcpkg.json`.
- Check that the patches in that directory still apply, and drop any that upstream has absorbed.
- Check `vcpkg_ports/libhdfs3/cmake/CMakeLists.txt` against upstream's source list (ClickHouse's
  `contrib/libhdfs3-cmake/CMakeLists.txt` is a good reference).
- Rebuild from scratch and run `make test`, `make test_unit`, `test/scripts/run_hdfs_tests.sh` and
  `test/scripts/run_kerberos_tests.sh` (the Integration Tests workflow runs all four).

## Patch inventory

The patches apply in the order `portfile.cmake` lists them. None has been offered upstream yet; each one that
ClickHouse/libhdfs3 takes is one less to carry. Bump `port-version` in `vcpkg.json` whenever a patch changes.

| Patch | Lines | What it does |
|---|---|---|
| `protobuf-uint8.patch` | 12 | Builds with current protobuf (`uint8` type). |
| `encryption-info.patch` | 138 | Parses each file's `FileEncryptionInfo`, and advertises the encryption-zones crypto protocol on create. |
| `config-update-merges.patch` | 14 | `Config::update` merges a second file (`hdfs-site.xml` after `core-site.xml`) instead of replacing. |
| `rename2.patch` | 183 | Exposes the NameNode's `rename2` RPC (atomic rename, optionally over an existing file). |
| `kerberos-no-stderr.patch` | 21 | Removes debug prints to stderr from the Kerberos connect path. |
| `wire-encryption.patch` | 2096 | NameNode RPC protection (SASL wrap), the DataNode SASL handshake, a DIGEST-MD5 client, AES/CTR transfer encryption, and `getDataEncryptionKey`. |
| `fewer-round-trips.patch` | 144 | Block locations carry the encryption info (no extra stat on open); lists each directory once; skips `getFsStats` on connect. |
| `peer-cache.patch` | 150 | Keeps several idle DataNode connections per DataNode, per file system. |
| `batched-listing.patch` | 331 | The `getBatchedListing` RPC (Hadoop 3.3+). |
| `wire-encryption-fixes.patch` | 626 | Fixes to `wire-encryption.patch`: AES keys unwrapped in the DataNode's order, DIGEST-MD5 sequence checks and constant-time MAC comparison, bounded read timeouts, reader lifetime across RPC reconnects, refetching an unknown data encryption key, typed handshake errors, privileged ports below 1024. |

To change a patch, apply them all to a git checkout of the pinned commit (one commit per patch), edit, and
regenerate the patch with `git diff` against the previous commit.

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
