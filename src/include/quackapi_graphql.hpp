#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

class DatabaseInstance;

//! Default row cap for thin GraphQL table selections (v0).
static constexpr idx_t QUACKAPI_GRAPHQL_DEFAULT_LIMIT = 100;
static constexpr idx_t QUACKAPI_GRAPHQL_MAX_ROOT_FIELDS = 16;
static constexpr idx_t QUACKAPI_GRAPHQL_MAX_COLUMNS_PER_FIELD = 64;
static constexpr idx_t QUACKAPI_GRAPHQL_MAX_TOTAL_FIELDS = 128;
static constexpr idx_t QUACKAPI_GRAPHQL_MAX_TOTAL_ROWS = 10000;
static constexpr idx_t QUACKAPI_GRAPHQL_MAX_DOCUMENT_BYTES = 64 * 1024;

//! Options for ExecuteGraphqlQuery / BuildGraphqlSchema.
//! When allowed_tables is null: built-in global allowlist rules. It is closed
//! when no table is registered unless the explicit legacy setting is enabled.
//! When non-null: only those tables (named CREATE GRAPHQL ROUTE mount).
struct GraphqlExecOptions {
	const vector<string> *allowed_tables = nullptr;
	idx_t limit = QUACKAPI_GRAPHQL_DEFAULT_LIMIT;
	//! Whether the caller completed a named-route authentication check.
	bool authenticated = false;
	//! Verified claim name -> string form, bound only to $claims_* policy params.
	const unordered_map<string, string> *claims = nullptr;
	//! Request-wide deadline and response cap propagated by the HTTP server.
	int64_t query_timeout_ms = 30000;
	idx_t max_response_bytes = 16 * 1024 * 1024;
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
//! Allowlist: CREATE GRAPHQL FOR TABLE … for built-in POST /graphql. An empty
//! registration is closed by default. `SET GLOBAL quackapi_graphql_allow_all = true`
//! explicitly restores legacy open-catalog behavior for migrations.
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
