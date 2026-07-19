#include "quackapi_server.hpp"

#include <cmath>

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/main/query_result.hpp"

#include "quackapi_auth.hpp"
#include "quackapi_openapi.hpp"
#include "quackapi_state.hpp"

#include "httplib.hpp"

namespace duckdb {

namespace {

string JsonEscape(const string &input) {
	string result;
	result.reserve(input.size() + 2);
	for (unsigned char c : input) {
		switch (c) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				result += buf;
			} else {
				result += static_cast<char>(c);
			}
		}
	}
	return result;
}

//! Render a DuckDB Value as JSON. The database typed the column — the JSON
//! representation follows the type, not string-formatting heuristics.
string ValueToJson(const Value &value) {
	if (value.IsNull()) {
		return "null";
	}
	auto &type = value.type();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return value.GetValue<bool>() ? "true" : "false";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::DECIMAL:
		return value.ToString();
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE: {
		auto d = value.GetValue<double>();
		if (std::isnan(d) || std::isinf(d)) {
			// JSON has no NaN/Infinity — mirror FastAPI/ujson and emit null
			return "null";
		}
		return value.ToString();
	}
	case LogicalTypeId::LIST: {
		auto &children = ListValue::GetChildren(value);
		string result = "[";
		for (idx_t i = 0; i < children.size(); i++) {
			if (i > 0) {
				result += ",";
			}
			result += ValueToJson(children[i]);
		}
		result += "]";
		return result;
	}
	case LogicalTypeId::STRUCT: {
		auto &children = StructValue::GetChildren(value);
		auto &child_types = StructType::GetChildTypes(type);
		string result = "{";
		for (idx_t i = 0; i < children.size(); i++) {
			if (i > 0) {
				result += ",";
			}
			result += "\"" + JsonEscape(child_types[i].first) + "\":" + ValueToJson(children[i]);
		}
		result += "}";
		return result;
	}
	default:
		return "\"" + JsonEscape(value.ToString()) + "\"";
	}
}

//! FastAPI-shaped validation error body.
string ValidationErrorJson(const string &loc_kind, const string &param_name, const string &msg, const string &type) {
	return "{\"detail\":[{\"loc\":[\"" + JsonEscape(loc_kind) + "\",\"" + JsonEscape(param_name) + "\"],\"msg\":\"" +
	       JsonEscape(msg) + "\",\"type\":\"" + JsonEscape(type) + "\"}]}";
}

struct RouteMatch {
	bool matched = false;
	QuackapiRoute route;
	// captured path params (name -> raw value)
	vector<std::pair<string, string>> path_params;
};

vector<string> SplitPath(const string &path) {
	vector<string> segments;
	string current;
	for (char c : path) {
		if (c == '/') {
			if (!current.empty()) {
				segments.push_back(current);
				current.clear();
			}
		} else {
			current += c;
		}
	}
	if (!current.empty()) {
		segments.push_back(current);
	}
	return segments;
}

//! Match a request path against a route pattern. ':name' and '{name}' segments
//! capture; all other segments must match exactly.
bool MatchPattern(const string &pattern, const string &path, vector<std::pair<string, string>> &captures) {
	auto pattern_segments = SplitPath(pattern);
	auto path_segments = SplitPath(path);
	if (pattern_segments.size() != path_segments.size()) {
		return false;
	}
	for (idx_t i = 0; i < pattern_segments.size(); i++) {
		auto &ps = pattern_segments[i];
		if (!ps.empty() && ps[0] == ':') {
			captures.emplace_back(ps.substr(1), path_segments[i]);
		} else if (ps.size() >= 2 && ps.front() == '{' && ps.back() == '}') {
			captures.emplace_back(ps.substr(1, ps.size() - 2), path_segments[i]);
		} else if (ps != path_segments[i]) {
			return false;
		}
	}
	return true;
}

void SetJson(duckdb_httplib::Response &res, int status, const string &body) {
	res.status = status;
	res.set_content(body, "application/json");
}

//! Response mode inferred from the single output column's name — the database
//! names the payload. `html` -> text/html, `text` -> text/plain; anything else
//! serializes as the JSON array of row objects.
enum class ResponseMode { JSON, HTML, TEXT };

ResponseMode ResponseModeFor(const vector<string> &names) {
	if (names.size() != 1) {
		return ResponseMode::JSON;
	}
	auto lower = StringUtil::Lower(names[0]);
	if (lower == "html") {
		return ResponseMode::HTML;
	}
	if (lower == "text") {
		return ResponseMode::TEXT;
	}
	return ResponseMode::JSON;
}

void SetInternalError(duckdb_httplib::Response &res, const string &server_side_detail) {
	// Never leak SQL/relation/path text to clients; log full detail server-side.
	fprintf(stderr, "quackapi: internal error: %s\n", server_side_detail.c_str());
	SetJson(res, 500, "{\"detail\":\"Internal Server Error\"}");
}

//! Collect request headers into a case-insensitive map (first value wins).
case_insensitive_map_t<string> CollectHeaders(const duckdb_httplib::Request &req) {
	case_insensitive_map_t<string> headers;
	for (auto &kv : req.headers) {
		if (headers.find(kv.first) == headers.end()) {
			headers[kv.first] = kv.second;
		}
	}
	return headers;
}

//! True if a prepared named parameter is a claims binding ($claims_<k>).
bool IsClaimsParam(const string &param_name, string &claim_key) {
	static const string prefix = "claims_";
	if (param_name.size() <= prefix.size()) {
		return false;
	}
	// named_param_map keys are without '$'. Match prefix case-insensitively;
	// claim key preserves the suffix casing from the SQL parameter name.
	if (!StringUtil::StartsWith(StringUtil::Lower(param_name), prefix)) {
		return false;
	}
	claim_key = param_name.substr(prefix.size());
	return !claim_key.empty();
}

//! True if `param_name` is a path-pattern capture (`:name` or `{name}`).
bool IsPathParam(const string &pattern, const string &param_name) {
	auto segments = SplitPath(pattern);
	for (auto &ps : segments) {
		if (!ps.empty() && ps[0] == ':' && ps.substr(1) == param_name) {
			return true;
		}
		if (ps.size() >= 2 && ps.front() == '{' && ps.back() == '}' &&
		    ps.substr(1, ps.size() - 2) == param_name) {
			return true;
		}
	}
	return false;
}

} // namespace

QuackapiHttpServer::QuackapiHttpServer(DatabaseInstance &db, const string &host_p, int port_p,
                                       const string &static_dir)
    : db_ptr(db.shared_from_this()), host(host_p), port(port_p) {
	server = make_uniq<duckdb_httplib::Server>();

	// Static files (FastAPI StaticFiles equivalent). httplib checks file
	// requests before route handlers, so API routes always win over files.
	if (!static_dir.empty() && !server->set_mount_point("/", static_dir)) {
		throw IOException("quackapi: static_dir \"%s\" is not a directory", static_dir);
	}

	server->new_task_queue = [] {
		return new duckdb_httplib::ThreadPool(32);
	};
	server->set_keep_alive_max_count(128);
	server->set_keep_alive_timeout(10);
	server->set_tcp_nodelay(true);
	// Cap request bodies to avoid unbounded memory DoS (httplib default is SIZE_MAX).
	server->set_payload_max_length(QUACKAPI_PAYLOAD_MAX_LENGTH);

	auto handler = [this](const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
		HandleRequest(req, res);
	};
	// Route matching happens against the live registry per request, so routes
	// created after quackapi_serve() are served immediately.
	server->Get(".*", handler);
	server->Post(".*", handler);
	server->Put(".*", handler);
	server->Delete(".*", handler);
	server->Patch(".*", handler);

	if (!server->is_valid()) {
		throw IOException("quackapi: failed to instantiate HTTP server for %s:%d", host, port);
	}
	is_running.store(true);

	// Bind synchronously so EADDRINUSE etc. propagate to quackapi_serve()
	if (!server->bind_to_port(host, port)) {
		throw IOException("quackapi: failed to bind to %s:%d (address in use, permission denied, or invalid host)",
		                  host, port);
	}
	listen_threads.emplace_back(ListenThread, this);
}

void QuackapiHttpServer::HandleRequest(const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
	auto db = db_ptr.lock();
	if (!db) {
		SetJson(res, 503, "{\"detail\":\"database shutting down\"}");
		return;
	}

	// Built-in docs routes (always present while serving; not in quackapi_routes()).
	// FastAPI parity: GET /openapi.json + GET /docs (+ optional /redoc).
	if (req.method == "GET" || req.method == "HEAD") {
		if (req.path == "/openapi.json") {
			string server_url = StringUtil::Format("http://%s:%d", host, port);
			try {
				auto doc = BuildOpenApiDocument(*db, server_url);
				SetJson(res, 200, doc);
			} catch (std::exception &ex) {
				SetInternalError(res, ex.what());
			} catch (...) {
				SetInternalError(res, "openapi generation failed");
			}
			if (req.method == "HEAD") {
				res.body.clear();
			}
			return;
		}
		if (req.path == "/docs" || req.path == "/docs/") {
			res.status = 200;
			res.set_content(OpenApiDocsHtml(), "text/html; charset=utf-8");
			if (req.method == "HEAD") {
				res.body.clear();
			}
			return;
		}
		if (req.path == "/redoc" || req.path == "/redoc/") {
			res.status = 200;
			res.set_content(OpenApiRedocHtml(), "text/html; charset=utf-8");
			if (req.method == "HEAD") {
				res.body.clear();
			}
			return;
		}
	}

	// Find a route: method + pattern
	RouteMatch match;
	bool path_matched_other_method = false;
	auto routes = QuackapiState::Get(*db).SnapshotRoutes();
	for (auto &route : routes) {
		vector<std::pair<string, string>> captures;
		if (MatchPattern(route.pattern, req.path, captures)) {
			if (route.method == req.method) {
				match.matched = true;
				match.route = route;
				match.path_params = std::move(captures);
				break;
			}
			path_matched_other_method = true;
		}
	}
	if (!match.matched) {
		if (path_matched_other_method) {
			SetJson(res, 405, "{\"detail\":\"Method Not Allowed\"}");
		} else {
			SetJson(res, 404, "{\"detail\":\"Not Found\"}");
		}
		return;
	}

	// ---- AUTH ENFORCEMENT (before prepare/execute) ----
	// Public routes (require_auth empty) pass through unchanged.
	// Auth is evaluated through the SQL surface (quackapi_verify_auth), the
	// same EvaluateAuthQuery shape quack uses for CONNECTION_REQUEST
	// (duckdb-quack src/quack_server.cpp). CREATE AUTH DDL remains the policy
	// definition layer; quack_authentication_function can point at
	// quackapi_authentication so the RPC plane shares that policy.
	auto headers = CollectHeaders(req);
	auto auth_result = CheckAuth(*db, match.route, headers);
	if (!auth_result.ok) {
		if (!auth_result.www_authenticate.empty()) {
			res.set_header("WWW-Authenticate", auth_result.www_authenticate);
		}
		SetJson(res, auth_result.status, auth_result.body);
		return;
	}
	// auth_result.claims is ready for $claims_<k> binding below.

	try {
		Connection con(*db);
		auto prepared = con.Prepare(match.route.handler_sql);
		if (prepared->HasError()) {
			SetInternalError(res, prepared->GetError());
			return;
		}

		// Request params: path captures shadow query params of the same name.
		case_insensitive_map_t<std::pair<string, string>> provided; // name -> (loc, raw)
		for (auto &kv : req.params) {
			provided[kv.first] = {"query", kv.second};
		}
		for (auto &kv : match.path_params) {
			provided[kv.first] = {"path", kv.second};
		}

		// The database types the columns: cast each provided param to the type
		// the prepared statement expects. Cast failure -> 422, FastAPI-shaped.
		// $claims_* params are server-verified: bind from claims or SQL NULL
		// (never 422 for a missing claim).
		auto expected_types = prepared->GetExpectedParameterTypes();
		case_insensitive_map_t<BoundParameterData> named_values;
		for (auto &entry : prepared->named_param_map) {
			auto &param_name = entry.first;

			string claim_key;
			if (IsClaimsParam(param_name, claim_key)) {
				auto cit = auth_result.claims.find(claim_key);
				if (cit == auth_result.claims.end()) {
					// Absent claim → SQL NULL (do NOT 422).
					named_values[param_name] = BoundParameterData(Value());
				} else {
					// Claims bind as VARCHAR; non-string/nested already JSON-encoded.
					named_values[param_name] = BoundParameterData(Value(cit->second));
				}
				continue;
			}

			auto it = provided.find(param_name);
			if (it == provided.end()) {
				const char *loc_kind =
				    IsPathParam(match.route.pattern, param_name) ? "path" : "query";
				SetJson(res, 422, ValidationErrorJson(loc_kind, param_name, "Field required", "missing"));
				return;
			}
			Value raw_value(it->second.second);
			auto type_it = expected_types.find(param_name);
			if (type_it != expected_types.end() && type_it->second.id() != LogicalTypeId::VARCHAR &&
			    type_it->second.id() != LogicalTypeId::UNKNOWN) {
				Value casted;
				string cast_error;
				if (!raw_value.DefaultTryCastAs(type_it->second, casted, &cast_error)) {
					SetJson(res, 422,
					        ValidationErrorJson(it->second.first, param_name,
					                            "Input should be a valid " + type_it->second.ToString(),
					                            "type_error"));
					return;
				}
				named_values[param_name] = BoundParameterData(casted);
			} else {
				named_values[param_name] = BoundParameterData(raw_value);
			}
		}

		auto result = prepared->Execute(named_values, false);
		if (result->HasError()) {
			// Request-parameter validation (missing / type cast) already returned
			// FastAPI-shaped 422 above. Any failure during Execute is a handler-side
			// error — sanitize to 500 so SQL text, error(), and internal messages
			// never reach clients (INVALID_INPUT would otherwise leak via 422).
			// Exception: Conversion Errors often mean a param was bound as VARCHAR
			// because DuckDB did not surface a concrete type (mixed $q + $n::INT).
			// Map those to 422 without leaking SQL / cast details (valsafe P1-1).
			auto err = result->GetError();
			auto err_lower = StringUtil::Lower(err);
			bool conversion = StringUtil::Contains(err_lower, "conversion error") ||
			                  StringUtil::Contains(err_lower, "could not convert string");
			if (conversion) {
				fprintf(stderr, "quackapi: param conversion at execute: %s\n", err.c_str());
				SetJson(res, 422,
				        ValidationErrorJson("query", "_", "Invalid input for parameter type", "type_error"));
			} else {
				SetInternalError(res, err);
			}
			return;
		}

		auto &names = result->names;
		auto mode = ResponseModeFor(names);

		// HTML/TEXT mode: a single column named `html`/`text` serves its raw
		// string value (e.g. SELECT tera_render(...) AS html). Multiple rows are
		// concatenated in order, so a query returning fragments streams a page.
		if (mode != ResponseMode::JSON) {
			string body;
			while (true) {
				auto chunk = result->Fetch();
				if (!chunk || chunk->size() == 0) {
					break;
				}
				for (idx_t row = 0; row < chunk->size(); row++) {
					auto value = chunk->GetValue(0, row);
					if (!value.IsNull()) {
						body += value.ToString();
					}
				}
			}
			res.status = match.route.status;
			res.set_content(body, mode == ResponseMode::HTML ? "text/html; charset=utf-8"
			                                                  : "text/plain; charset=utf-8");
			return;
		}

		// Serialize: JSON array of row objects, typed by the query's columns.
		string body = "[";
		bool first_row = true;
		while (true) {
			auto chunk = result->Fetch();
			if (!chunk || chunk->size() == 0) {
				break;
			}
			for (idx_t row = 0; row < chunk->size(); row++) {
				if (!first_row) {
					body += ",";
				}
				first_row = false;
				body += "{";
				for (idx_t col = 0; col < chunk->ColumnCount(); col++) {
					if (col > 0) {
						body += ",";
					}
					body += "\"" + JsonEscape(names[col]) + "\":" + ValueToJson(chunk->GetValue(col, row));
				}
				body += "}";
			}
		}
		body += "]";
		SetJson(res, match.route.status, body);
	} catch (std::exception &ex) {
		SetInternalError(res, ex.what());
	} catch (...) {
		SetInternalError(res, "unknown exception");
	}
}

void QuackapiHttpServer::ListenThread(QuackapiHttpServer *server) {
	// Never let an exception escape a listener thread — that would terminate
	// the host process.
	try {
		server->server->listen_after_bind();
	} catch (...) {
		server->is_running.store(false);
	}
}

void QuackapiHttpServer::StopAccepting() {
	// load/store: is_running is touched by ctor, listener, and stop threads.
	if (is_running.exchange(false)) {
		server->stop();
	}
}

void QuackapiHttpServer::Close() {
	StopAccepting();
	for (auto &thread : listen_threads) {
		if (thread.joinable()) {
			thread.join();
		}
	}
}

QuackapiHttpServer::~QuackapiHttpServer() {
	try {
		Close();
	} catch (std::exception &) {
	}
}

} // namespace duckdb
