# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(quackapi
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)
# DuckDB 2.0 links an extension into libduckdb, the shell and unittest only when it is listed here; without it
# `LOAD quackapi` and `require quackapi` look for an installed copy instead of the one this tree built.
duckdb_extension_statically_link(quackapi)

# JSON is used by tests and by handlers returning JSON
duckdb_extension_load(json)
