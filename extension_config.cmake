# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(hdfs
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# Since DuckDB v2.0, duckdb_extension_load only builds the extension; link it
# into the shell and unittest binaries too.
duckdb_extension_statically_link(hdfs)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
