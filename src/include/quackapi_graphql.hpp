#pragma once

#include "duckdb/common/string.hpp"

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
string ExecuteGraphqlQuery(DatabaseInstance &db, const string &query,
                           idx_t limit = QUACKAPI_GRAPHQL_DEFAULT_LIMIT);

//! Extract the "query" string from a GraphQL-over-HTTP JSON body.
//! Returns false with a GraphQL-ish error JSON when body is unusable.
bool GraphqlExtractQuery(DatabaseInstance &db, const string &raw_body, string &query_out, string &error_json);

//! GET /graphql/schema — JSON catalog of main-schema tables → column names.
//! Not full GraphQL __schema; enough for clients to discover selectable fields.
string BuildGraphqlSchema(DatabaseInstance &db);

} // namespace duckdb
