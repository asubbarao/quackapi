#pragma once

#include <cstdint>
#include <mutex>
#include <utility>

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb {

class DatabaseInstance;
class Connection;

//! Which point in a matched route's request lifecycle executes middleware.
enum class QuackapiMiddlewarePhase : uint8_t {
	BEFORE = 0,
	AFTER = 1,
};

//! A SQL middleware definition. `group_name` empty means every route.
//! Definitions are kept in their own ObjectCache entry so separately-loaded
//! extension copies share one registry for each DatabaseInstance.
struct QuackapiMiddleware {
	string name;
	QuackapiMiddlewarePhase phase = QuackapiMiddlewarePhase::BEFORE;
	string group_name;
	string handler_sql;
	//! Stable creation order. CREATE OR REPLACE retains this value.
	uint64_t registration_order = 0;
};

//! Request/response values exposed to the middleware executor. Server code
//! populates request fields after routing and authentication, and applies the
//! mutable response fields if ExecuteQuackapiMiddleware returns false.
struct QuackapiMiddlewareContext {
	string method;
	string path;
	string route_name;
	string group_name;
	string request_id;
	string request_body;
	string headers_json;
	string query_json;
	string client_ip;
	string auth_subject;
	//! The response status/body visible to AFTER middleware. For BEFORE hooks,
	//! status is 0 until the route produces a response.
	int32_t status = 0;
	string response_body;
	vector<std::pair<string, string>> response_headers;
	//! Total request deadline selected by quackapi_serve. The executor always
	//! inherits a tighter active deadline when one is already installed.
	int64_t timeout_ms = 30000;
	int64_t elapsed_ms = 0;
	//! Set only for a malformed or failing hook. Its error never reaches HTTP.
	bool middleware_failed = false;
};

//! Middleware SQL receives only named prepared-statement parameters; server
//! code must never interpolate request values into handler_sql.
//!
//! Available in both phases: $request_id, $method, $path, $route, $group,
//! $client_ip, $auth_subject, $headers_json, $query_json, $body.
//! AFTER additionally receives $status and $elapsed_ms. Values that are not
//! available (for example $auth_subject on a public route) bind SQL NULL.
//!
//! A middleware may return zero rows for a no-op/audit-only statement. Rows it
//! does return must use these typed control columns:
//!   allow        BOOLEAN NOT NULL (false stops the request)
//!   status       INTEGER NULL     (valid HTTP status when allow=false)
//!   body         VARCHAR NULL     (response body when allow=false)
//!   header_name  VARCHAR NULL
//!   header_value VARCHAR NULL
//! `header_name` and `header_value` must be present together. Header rows may
//! be emitted alongside a control row. The server validates this contract and
//! fails closed on invalid results or execution errors.
constexpr idx_t QUACKAPI_MIDDLEWARE_MAX_SQL_BYTES = 16 * 1024;
constexpr idx_t QUACKAPI_MIDDLEWARE_MAX_DEFINITIONS = 64;
constexpr idx_t QUACKAPI_MIDDLEWARE_MAX_RESULT_ROWS = 64;
constexpr idx_t QUACKAPI_MIDDLEWARE_MAX_HEADER_BYTES = 8 * 1024;
constexpr idx_t QUACKAPI_MIDDLEWARE_MAX_BODY_BYTES = 1024 * 1024;

string QuackapiMiddlewarePhaseName(QuackapiMiddlewarePhase phase);

//! Database-scoped middleware registry. This deliberately does not live in
//! QuackapiState: middleware has its own lifecycle and remains collision-free
//! if a static and a loadable copy of the extension are both present.
class QuackapiMiddlewareRegistry : public ObjectCacheEntry {
public:
	static constexpr const char *CACHE_KEY = "quackapi_middleware_registry";

	static string ObjectType() {
		return "quackapi_middleware_registry";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		// DDL-defined behavior must not disappear under object-cache pressure.
		return optional_idx();
	}

	static QuackapiMiddlewareRegistry &Get(DatabaseInstance &db);

	//! CREATE [OR REPLACE] MIDDLEWARE. Replacing preserves registration order.
	void Add(const QuackapiMiddleware &middleware, bool or_replace);
	//! DROP MIDDLEWARE. Returns false if absent.
	bool Drop(const string &name);
	//! Applicable snapshot in deterministic lifecycle order. BEFORE runs global
	//! then group middleware; AFTER runs group then global middleware.
	vector<QuackapiMiddleware> Snapshot(QuackapiMiddlewarePhase phase, const string &group_name);
	//! Full registration-order snapshot for quackapi_middlewares().
	vector<QuackapiMiddleware> SnapshotAll();

private:
	std::mutex mutex;
	vector<QuackapiMiddleware> middlewares;
	uint64_t next_registration_order = 1;
};

//! Parser extension for CREATE [OR REPLACE] / DROP MIDDLEWARE.
class MiddlewareDdlParserExtension : public ParserExtension {
public:
	MiddlewareDdlParserExtension();
};

//! Internal DDL application table function (registered by the extension).
TableFunction GetApplyMiddlewareFunction();
//! Introspection: name, phase, group_name, handler_sql, registration_order.
TableFunction GetQuackapiMiddlewaresFunction();

//! Execute matching SQL middleware in snapshot order. The caller must enforce
//! route authentication before this function; it receives only verified claims.
//!
//! Returns true when all hooks allow the next lifecycle stage. Returns false
//! when a hook blocks the request or when execution/output validation fails;
//! `context.status`/`response_body` then contain a safe response and
//! `context.middleware_failed` distinguishes a 500 from a declared block.
bool ExecuteQuackapiMiddleware(Connection &connection, DatabaseInstance &db, QuackapiMiddlewarePhase phase,
                               const string &group_name, QuackapiMiddlewareContext &context, bool authenticated,
                               const unordered_map<string, string> &claims);

} // namespace duckdb
