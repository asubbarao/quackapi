#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

class DatabaseInstance;

//! Default row cap for thin GraphQL table selections (v0).
static constexpr idx_t QUACKAPI_GRAPHQL_DEFAULT_LIMIT = 100;

//! Thin GraphQL v0 — catalog-only table selection.
//!
//! Accepts a GraphQL document of the form:
//!   query { tableName { col1 col2 } }
//!   { tableName { col1 col2 } }
//! Maps each root field to:
//!   SELECT "col1", "col2" FROM "tableName" LIMIT N
//! Response shape:
//!   { "data": { "tableName": [ {…}, … ] } }
//!   or { "errors": [ { "message": "…" } ] }
//!
//! No mutations, nested joins, fragments, arguments, aliases, or full grammar.
//! Schema source is the DuckDB catalog only. Future work may feed types from
//! sitting_duck / parser_tools over external code; v0 does not.
//!
//! Allowlist (optional): CREATE GRAPHQL FOR TABLE … registers tables for the
//! built-in POST /graphql. Empty allowlist = open (all main tables, default).
//! Non-empty = only registered tables.
string ExecuteGraphqlQuery(DatabaseInstance &db, const string &query, idx_t limit = QUACKAPI_GRAPHQL_DEFAULT_LIMIT);

//! Extract the "query" string from a GraphQL-over-HTTP JSON body.
//! Returns false with a GraphQL-ish error JSON when body is unusable.
bool GraphqlExtractQuery(DatabaseInstance &db, const string &raw_body, string &query_out, string &error_json);

//! GET /graphql/schema — JSON catalog of main-schema tables → column names.
//! Respects the GraphQL allowlist when active. Not full GraphQL __schema.
string BuildGraphqlSchema(DatabaseInstance &db);

//! `CREATE [OR REPLACE] GRAPHQL FOR TABLE <table> [, …]`
//! `DROP GRAPHQL FOR TABLE <table> [, …]`
//! `DROP GRAPHQL ALL`
//!
//! Registers main-schema tables/views for the built-in thin GraphQL endpoint.
//! Does not mount a new path — still POST /graphql. Named path mounts are
//! design-only: CREATE GRAPHQL ROUTE (see docs/guide/graphql-v0.md).
class GraphqlDdlParserExtension : public ParserExtension {
public:
	GraphqlDdlParserExtension();
};

//! Register quackapi_graphql_tables() inspection TF.
void RegisterQuackapiGraphqlFunctions(ExtensionLoader &loader);

} // namespace duckdb
