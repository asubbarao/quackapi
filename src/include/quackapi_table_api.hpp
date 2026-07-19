#pragma once

#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

//! `CREATE API FOR TABLE <table> [AT '<base_path>'] [KEY '<column>']`
//!
//! Generates read routes from a table/view at execution time (PostgREST-style):
//!   GET  <base>            -> SELECT * FROM <table>
//!   GET  <base>/:<key>     -> SELECT * FROM <table> WHERE <key> = $<key>
//!
//! base_path defaults to '/<table>', key defaults to 'id'. Read-only in this
//! version; the generated routes are ordinary registry rows, so `quackapi_routes()`
//! shows them and `DROP ROUTE` removes them individually.
class TableApiDdlParserExtension : public ParserExtension {
public:
	TableApiDdlParserExtension();
};

} // namespace duckdb
