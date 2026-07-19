# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(quackapi
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# JSON is used by tests and by handlers returning JSON
duckdb_extension_load(json)
