#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

class DatabaseInstance;

//! Default row cap for thin GraphQL table selections (v0).
static constexpr idx_t QUACKAPI_GRAPHQL_DEFAULT_LIMIT = 100;

//! Options for ExecuteGraphqlQuery / BuildGraphqlSchema.
//! When allowed_tables is null: built-in global allowlist rules (open or FOR TABLE).
//! When non-null: only those tables (named CREATE GRAPHQL ROUTE mount).
struct GraphqlExecOptions {
	const vector<string> *allowed_tables = nullptr;
	idx_t limit = QUACKAPI_GRAPHQL_DEFAULT_LIMIT;
	//! Schema JSON "mode": "open" | "allowlist" | "route"
	string mode;
	//! Optional route name for schema note when mode is "route".
	string route_name;
};

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
//! Schema source is the DuckDB catalog only.
//!
//! Allowlist (optional): CREATE GRAPHQL FOR TABLE … for built-in POST /graphql.
//! Named mounts: CREATE GRAPHQL ROUTE … POST '/path' FROM …
string ExecuteGraphqlQuery(DatabaseInstance &db, const string &query,
                           const GraphqlExecOptions &options = GraphqlExecOptions {});

//! Convenience: global allowlist + default/optional limit (built-in /graphql).
inline string ExecuteGraphqlQuery(DatabaseInstance &db, const string &query, idx_t limit) {
	GraphqlExecOptions opts;
	opts.limit = limit;
	return ExecuteGraphqlQuery(db, query, opts);
}

//! Extract the "query" string from a GraphQL-over-HTTP JSON body.
//! Returns false with a GraphQL-ish error JSON when body is unusable.
bool GraphqlExtractQuery(DatabaseInstance &db, const string &raw_body, string &query_out, string &error_json);

//! Catalog schema JSON. options.allowed_tables null → global open/allowlist;
//! non-null → only those tables (mode should be "route").
string BuildGraphqlSchema(DatabaseInstance &db, const GraphqlExecOptions &options = GraphqlExecOptions {});

//! `CREATE [OR REPLACE] GRAPHQL FOR TABLE <table> [, …]`
//! `DROP GRAPHQL FOR TABLE <table> [, …]`
//! `DROP GRAPHQL ALL`
//! `CREATE [OR REPLACE] GRAPHQL ROUTE <name> POST '<path>' FROM … [REQUIRE …] [LIMIT n]`
//! `DROP GRAPHQL ROUTE <name>`
class GraphqlDdlParserExtension : public ParserExtension {
public:
	GraphqlDdlParserExtension();
};

//! Register quackapi_graphql_tables() + quackapi_graphql_routes() inspection TFs.
void RegisterQuackapiGraphqlFunctions(ExtensionLoader &loader);

} // namespace duckdb
