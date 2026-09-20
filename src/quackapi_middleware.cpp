#include "quackapi_middleware.hpp"

#include <chrono>

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"

#include "quackapi_limits.hpp"
#include "quackapi_policy.hpp"
#include "quackapi_util.hpp"

namespace duckdb {

namespace {

bool IsMiddlewareName(const string &name) {
	if (name.empty() || name.size() > 128) {
		return false;
	}
	for (auto c : name) {
		bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		bool digit = c >= '0' && c <= '9';
		if (!alpha && !digit && c != '_' && c != '-') {
			return false;
		}
	}
	return true;
}

bool IsWhitespaceBoundary(const string &s, idx_t pos) {
	return pos >= s.size() || StringUtil::CharacterIsSpace(s[pos]);
}

//! Finds a standalone keyword before the SQL body. The grammar's prefix has
//! no quoted values, so a whitespace boundary is both simpler and stricter
//! than treating SQL text as an ad-hoc string language.
idx_t FindStandaloneKeyword(const string &upper, const string &keyword, idx_t start) {
	for (idx_t pos = start; pos + keyword.size() <= upper.size(); pos++) {
		if (upper.compare(pos, keyword.size(), keyword) == 0 &&
		    (pos == 0 || StringUtil::CharacterIsSpace(upper[pos - 1])) &&
		    IsWhitespaceBoundary(upper, pos + keyword.size())) {
			return pos;
		}
	}
	return string::npos;
}

vector<string> WhitespaceTokens(const string &input) {
	vector<string> result;
	idx_t pos = 0;
	while (pos < input.size()) {
		while (pos < input.size() && StringUtil::CharacterIsSpace(input[pos])) {
			pos++;
		}
		idx_t begin = pos;
		while (pos < input.size() && !StringUtil::CharacterIsSpace(input[pos])) {
			pos++;
		}
		if (begin < pos) {
			result.push_back(input.substr(begin, pos - begin));
		}
	}
	return result;
}

struct MiddlewareDdlParseData : public ParserExtensionParseData {
	string action; // CREATE or DROP
	bool or_replace = false;
	QuackapiMiddleware middleware;

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto copy = make_uniq<MiddlewareDdlParseData>();
		copy->action = action;
		copy->or_replace = or_replace;
		copy->middleware = middleware;
		return std::move(copy);
	}
	string ToString() const override {
		return action + " MIDDLEWARE " + middleware.name;
	}
};

//! Grammar:
//!   CREATE [OR REPLACE] MIDDLEWARE <name> BEFORE|AFTER [GROUP <name>] AS <sql>
//!   DROP MIDDLEWARE <name>
ParserExtensionParseResult MiddlewareDdlParse(ParserExtensionInfo *, const string &query) {
	auto q = QuackapiDdlTrim(query);
	auto upper = StringUtil::Upper(q);

	if (StringUtil::StartsWith(upper, "DROP MIDDLEWARE ")) {
		auto name = QuackapiDdlTrim(q.substr(16));
		if (!IsMiddlewareName(name)) {
			return ParserExtensionParseResult("DROP MIDDLEWARE expects a single name ([A-Za-z0-9_-], max 128 bytes)");
		}
		auto data = make_uniq<MiddlewareDdlParseData>();
		data->action = "DROP";
		data->middleware.name = name;
		return ParserExtensionParseResult(std::move(data));
	}

	bool or_replace = false;
	idx_t prefix_end = 0;
	if (StringUtil::StartsWith(upper, "CREATE OR REPLACE MIDDLEWARE ")) {
		or_replace = true;
		prefix_end = 29;
	} else if (StringUtil::StartsWith(upper, "CREATE MIDDLEWARE ")) {
		prefix_end = 18;
	} else {
		// Not ours; leave normal DuckDB parsing/error reporting intact.
		return ParserExtensionParseResult();
	}

	auto as_pos = FindStandaloneKeyword(upper, "AS", prefix_end);
	if (as_pos == string::npos) {
		return ParserExtensionParseResult("CREATE MIDDLEWARE <name> BEFORE|AFTER [GROUP <name>] AS <sql>");
	}
	auto declaration = QuackapiDdlTrim(q.substr(prefix_end, as_pos - prefix_end));
	auto tokens = WhitespaceTokens(declaration);
	if (tokens.size() != 2 && tokens.size() != 4) {
		return ParserExtensionParseResult("CREATE MIDDLEWARE expects <name> BEFORE|AFTER [GROUP <name>] before AS");
	}
	if (!IsMiddlewareName(tokens[0])) {
		return ParserExtensionParseResult("Middleware name must use [A-Za-z0-9_-] and be at most 128 bytes");
	}
	auto phase = StringUtil::Upper(tokens[1]);
	if (phase != "BEFORE" && phase != "AFTER") {
		return ParserExtensionParseResult("MIDDLEWARE phase must be BEFORE or AFTER");
	}
	string group_name;
	if (tokens.size() == 4) {
		if (!StringUtil::CIEquals(tokens[2], "GROUP") || !IsMiddlewareName(tokens[3])) {
			return ParserExtensionParseResult("MIDDLEWARE GROUP expects a name using [A-Za-z0-9_-]");
		}
		group_name = tokens[3];
	}
	auto handler_sql = QuackapiDdlTrim(q.substr(as_pos + 2));
	if (handler_sql.empty()) {
		return ParserExtensionParseResult("MIDDLEWARE AS requires SQL");
	}
	if (handler_sql.size() > QUACKAPI_MIDDLEWARE_MAX_SQL_BYTES) {
		return ParserExtensionParseResult("MIDDLEWARE SQL exceeds the 16 KiB definition limit");
	}

	auto data = make_uniq<MiddlewareDdlParseData>();
	data->action = "CREATE";
	data->or_replace = or_replace;
	data->middleware.name = tokens[0];
	data->middleware.phase = phase == "BEFORE" ? QuackapiMiddlewarePhase::BEFORE : QuackapiMiddlewarePhase::AFTER;
	data->middleware.group_name = group_name;
	data->middleware.handler_sql = handler_sql;
	return ParserExtensionParseResult(std::move(data));
}

struct ApplyMiddlewareBindData : public TableFunctionData {
	string action;
	bool or_replace = false;
	QuackapiMiddleware middleware;
	bool finished = false;
};

unique_ptr<FunctionData> ApplyMiddlewareBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ApplyMiddlewareBindData>();
	bind_data->action = input.inputs[0].GetValue<string>();
	bind_data->or_replace = input.inputs[1].GetValue<bool>();
	bind_data->middleware.name = input.inputs[2].GetValue<string>();
	bind_data->middleware.phase = input.inputs[3].GetValue<string>() == "AFTER" ? QuackapiMiddlewarePhase::AFTER
	                                                                            : QuackapiMiddlewarePhase::BEFORE;
	bind_data->middleware.group_name = input.inputs[4].GetValue<string>();
	bind_data->middleware.handler_sql = input.inputs[5].GetValue<string>();
	BindStatusColumn(return_types, names);
	return std::move(bind_data);
}

void ApplyMiddlewareExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ApplyMiddlewareBindData>();
	if (bind_data.finished) {
		return;
	}
	auto &registry = QuackapiMiddlewareRegistry::Get(*context.db);
	string message;
	if (bind_data.action == "CREATE") {
		if (!IsMiddlewareName(bind_data.middleware.name) ||
		    (!bind_data.middleware.group_name.empty() && !IsMiddlewareName(bind_data.middleware.group_name))) {
			throw InvalidInputException("Middleware names must use [A-Za-z0-9_-] and be at most 128 bytes");
		}
		if (bind_data.middleware.handler_sql.empty() ||
		    bind_data.middleware.handler_sql.size() > QUACKAPI_MIDDLEWARE_MAX_SQL_BYTES) {
			throw InvalidInputException("Middleware SQL must be between 1 byte and 16 KiB");
		}
		// Compile on CREATE so malformed SQL is never registered. Preparation does
		// not execute the statement; request values bind later through DuckDB's
		// named-parameter API, never SQL string construction.
		Connection con(*context.db);
		auto prepared = con.Prepare(bind_data.middleware.handler_sql);
		if (prepared->HasError()) {
			throw InvalidInputException("Invalid handler SQL for middleware \"%s\": %s", bind_data.middleware.name,
			                            prepared->GetError());
		}
		registry.Add(bind_data.middleware, bind_data.or_replace);
		message = StringUtil::Format(
		    "Middleware %s: %s%s", bind_data.middleware.name, QuackapiMiddlewarePhaseName(bind_data.middleware.phase),
		    bind_data.middleware.group_name.empty() ? "" : " GROUP " + bind_data.middleware.group_name);
	} else if (registry.Drop(bind_data.middleware.name)) {
		message = StringUtil::Format("Dropped middleware %s", bind_data.middleware.name);
	} else {
		throw InvalidInputException("Middleware \"%s\" does not exist", bind_data.middleware.name);
	}
	EmitOneShotStatus(output, bind_data.finished, message);
}

TableFunction MakeApplyMiddlewareFunction() {
	return MakeApplyDdlFunction("quackapi_apply_middleware",
	                            {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                             LogicalType::VARCHAR, LogicalType::VARCHAR},
	                            ApplyMiddlewareExec, ApplyMiddlewareBind);
}

ParserExtensionPlanResult MiddlewareDdlPlan(ParserExtensionInfo *, ClientContext &,
                                            unique_ptr<ParserExtensionParseData> parse_data) {
	auto &data = static_cast<MiddlewareDdlParseData &>(*parse_data);
	ParserExtensionPlanResult result;
	result.function = MakeApplyMiddlewareFunction();
	result.parameters.push_back(Value(data.action));
	result.parameters.push_back(Value::BOOLEAN(data.or_replace));
	result.parameters.push_back(Value(data.middleware.name));
	result.parameters.push_back(Value(QuackapiMiddlewarePhaseName(data.middleware.phase)));
	result.parameters.push_back(Value(data.middleware.group_name));
	result.parameters.push_back(Value(data.middleware.handler_sql));
	FinishDdlPlan(result);
	return result;
}

struct MiddlewaresBindData : public TableFunctionData {};

struct MiddlewaresGlobalState : public GlobalTableFunctionState {
	vector<QuackapiMiddleware> middlewares;
	idx_t offset = 0;
};

unique_ptr<FunctionData> MiddlewaresBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                         vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("phase");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("group_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("handler_sql");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("registration_order");
	return make_uniq<MiddlewaresBindData>();
}

unique_ptr<GlobalTableFunctionState> MiddlewaresInit(ClientContext &context, TableFunctionInitInput &) {
	auto state = make_uniq<MiddlewaresGlobalState>();
	state->middlewares = QuackapiMiddlewareRegistry::Get(*context.db).SnapshotAll();
	return std::move(state);
}

void MiddlewaresExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<MiddlewaresGlobalState>();
	idx_t row = 0;
	while (state.offset < state.middlewares.size() && row < STANDARD_VECTOR_SIZE) {
		auto &middleware = state.middlewares[state.offset++];
		output.SetValue(0, row, Value(middleware.name));
		output.SetValue(1, row, Value(QuackapiMiddlewarePhaseName(middleware.phase)));
		output.SetValue(2, row, Value(middleware.group_name));
		output.SetValue(3, row, Value(middleware.handler_sql));
		output.SetValue(4, row, Value::UBIGINT(middleware.registration_order));
		row++;
	}
	output.SetCardinality(row);
}

bool IsClaimsParameter(const string &name, string &claim_key) {
	auto lower = StringUtil::Lower(name);
	if (!StringUtil::StartsWith(lower, "claims_") || name.size() == 7) {
		return false;
	}
	claim_key = name.substr(7);
	return true;
}

bool IsValidHttpStatus(int32_t status) {
	return status >= 100 && status <= 599;
}

bool IsHttpTokenChar(char c) {
	bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	bool digit = c >= '0' && c <= '9';
	return alpha || digit || c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' ||
	       c == '+' || c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}

bool IsForbiddenResponseHeader(const string &name) {
	auto lower = StringUtil::Lower(name);
	return lower == "connection" || lower == "content-length" || lower == "keep-alive" ||
	       lower == "proxy-authenticate" || lower == "proxy-authorization" || lower == "te" || lower == "trailer" ||
	       lower == "transfer-encoding" || lower == "upgrade";
}

bool IsSafeResponseHeader(const string &name, const string &value) {
	if (name.empty() || name.size() > 256 || value.size() > QUACKAPI_MIDDLEWARE_MAX_HEADER_BYTES ||
	    name.size() + value.size() > QUACKAPI_MIDDLEWARE_MAX_HEADER_BYTES || IsForbiddenResponseHeader(name)) {
		return false;
	}
	for (auto c : name) {
		if (!IsHttpTokenChar(c)) {
			return false;
		}
	}
	return value.find('\r') == string::npos && value.find('\n') == string::npos;
}

void SetMiddlewareFailure(QuackapiMiddlewareContext &context, int32_t status = 500) {
	context.status = status;
	context.response_body =
	    status == 408 ? "{\"detail\":\"Request timed out\"}" : "{\"detail\":\"Middleware execution failed\"}";
	context.middleware_failed = true;
}

bool GetMiddlewareResultColumns(const QueryResult &result, idx_t &allow_col, idx_t &status_col, idx_t &body_col,
                                idx_t &header_name_col, idx_t &header_value_col) {
	auto invalid = result.names.size();
	allow_col = status_col = body_col = header_name_col = header_value_col = invalid;
	for (idx_t col = 0; col < result.names.size(); col++) {
		auto lower = StringUtil::Lower(result.names[col]);
		idx_t *slot = nullptr;
		if (lower == "allow") {
			slot = &allow_col;
		} else if (lower == "status") {
			slot = &status_col;
		} else if (lower == "body") {
			slot = &body_col;
		} else if (lower == "header_name") {
			slot = &header_name_col;
		} else if (lower == "header_value") {
			slot = &header_value_col;
		}
		if (slot) {
			if (*slot != invalid) {
				return false; // duplicate control column, even with casing variation
			}
			*slot = col;
		}
	}
	if (allow_col == invalid || allow_col >= result.types.size() ||
	    result.types[allow_col].id() != LogicalTypeId::BOOLEAN) {
		return false;
	}
	if ((status_col != invalid &&
	     (status_col >= result.types.size() || result.types[status_col].id() != LogicalTypeId::INTEGER)) ||
	    (body_col != invalid &&
	     (body_col >= result.types.size() || result.types[body_col].id() != LogicalTypeId::VARCHAR)) ||
	    (header_name_col != invalid &&
	     (header_name_col >= result.types.size() || result.types[header_name_col].id() != LogicalTypeId::VARCHAR)) ||
	    (header_value_col != invalid &&
	     (header_value_col >= result.types.size() || result.types[header_value_col].id() != LogicalTypeId::VARCHAR))) {
		return false;
	}
	return (header_name_col == invalid) == (header_value_col == invalid);
}

} // namespace

string QuackapiMiddlewarePhaseName(QuackapiMiddlewarePhase phase) {
	return phase == QuackapiMiddlewarePhase::AFTER ? "AFTER" : "BEFORE";
}

QuackapiMiddlewareRegistry &QuackapiMiddlewareRegistry::Get(DatabaseInstance &db) {
	auto state = db.GetObjectCache().GetOrCreate<QuackapiMiddlewareRegistry>(CACHE_KEY);
	if (!state) {
		throw InternalException("quackapi: failed to create middleware registry");
	}
	return *state;
}

void QuackapiMiddlewareRegistry::Add(const QuackapiMiddleware &middleware, bool or_replace) {
	std::lock_guard<std::mutex> lock(mutex);
	for (auto it = middlewares.begin(); it != middlewares.end(); ++it) {
		if (it->name == middleware.name) {
			if (!or_replace) {
				throw InvalidInputException("Middleware \"%s\" already exists — use CREATE OR REPLACE MIDDLEWARE",
				                            middleware.name);
			}
			auto replacement = middleware;
			replacement.registration_order = it->registration_order;
			*it = std::move(replacement);
			return;
		}
	}
	if (middlewares.size() >= QUACKAPI_MIDDLEWARE_MAX_DEFINITIONS) {
		throw InvalidInputException("Middleware registry limit is %llu definitions",
		                            (unsigned long long)QUACKAPI_MIDDLEWARE_MAX_DEFINITIONS);
	}
	auto added = middleware;
	added.registration_order = next_registration_order++;
	middlewares.push_back(std::move(added));
}

bool QuackapiMiddlewareRegistry::Drop(const string &name) {
	std::lock_guard<std::mutex> lock(mutex);
	for (auto it = middlewares.begin(); it != middlewares.end(); ++it) {
		if (it->name == name) {
			middlewares.erase(it);
			return true;
		}
	}
	return false;
}

vector<QuackapiMiddleware> QuackapiMiddlewareRegistry::Snapshot(QuackapiMiddlewarePhase phase,
                                                                const string &group_name) {
	std::lock_guard<std::mutex> lock(mutex);
	vector<QuackapiMiddleware> result;
	result.reserve(middlewares.size());
	auto append_scope = [&](bool global) {
		for (auto &middleware : middlewares) {
			if (middleware.phase == phase &&
			    (global ? middleware.group_name.empty() : middleware.group_name == group_name)) {
				result.push_back(middleware);
			}
		}
	};
	if (phase == QuackapiMiddlewarePhase::BEFORE) {
		append_scope(true);
		if (!group_name.empty()) {
			append_scope(false);
		}
	} else {
		if (!group_name.empty()) {
			append_scope(false);
		}
		append_scope(true);
	}
	return result;
}

vector<QuackapiMiddleware> QuackapiMiddlewareRegistry::SnapshotAll() {
	std::lock_guard<std::mutex> lock(mutex);
	return middlewares;
}

bool ExecuteQuackapiMiddleware(Connection &connection, DatabaseInstance &db, QuackapiMiddlewarePhase phase,
                               const string &group_name, QuackapiMiddlewareContext &context, bool authenticated,
                               const unordered_map<string, string> &claims) {
	try {
		auto middleware = QuackapiMiddlewareRegistry::Get(db).Snapshot(phase, group_name);
		for (auto &definition : middleware) {
			bool deny_unauthenticated = false;
			string policy_error;
			auto handler_sql = RewriteHandlerWithPolicies(db, definition.handler_sql, authenticated,
			                                              deny_unauthenticated, policy_error);
			if (deny_unauthenticated) {
				context.status = 401;
				context.response_body = "{\"detail\":\"Not authenticated\"}";
				return false;
			}
			if (!policy_error.empty()) {
				SetMiddlewareFailure(context);
				return false;
			}

			auto prepared = connection.Prepare(handler_sql);
			if (prepared->HasError()) {
				SetMiddlewareFailure(context);
				return false;
			}

			case_insensitive_map_t<BoundParameterData> values;
			for (auto &entry : prepared->named_param_map) {
				auto &name = entry.first;
				auto lower = StringUtil::Lower(name);
				string claim_key;
				if (IsClaimsParameter(name, claim_key)) {
					auto claim = claims.find(claim_key);
					values[name] =
					    claim == claims.end() ? BoundParameterData(Value()) : BoundParameterData(Value(claim->second));
				} else if (lower == "request_id") {
					values[name] = BoundParameterData(Value(context.request_id));
				} else if (lower == "method") {
					values[name] = BoundParameterData(Value(context.method));
				} else if (lower == "path") {
					values[name] = BoundParameterData(Value(context.path));
				} else if (lower == "route") {
					values[name] = BoundParameterData(Value(context.route_name));
				} else if (lower == "group") {
					values[name] = context.group_name.empty() ? BoundParameterData(Value())
					                                          : BoundParameterData(Value(context.group_name));
				} else if (lower == "client_ip") {
					values[name] = context.client_ip.empty() ? BoundParameterData(Value())
					                                         : BoundParameterData(Value(context.client_ip));
				} else if (lower == "auth_subject") {
					values[name] = context.auth_subject.empty() ? BoundParameterData(Value())
					                                            : BoundParameterData(Value(context.auth_subject));
				} else if (lower == "headers_json") {
					values[name] = BoundParameterData(Value(context.headers_json));
				} else if (lower == "query_json") {
					values[name] = BoundParameterData(Value(context.query_json));
				} else if (lower == "body") {
					values[name] = BoundParameterData(Value(context.request_body));
				} else if (lower == "status") {
					values[name] = context.status == 0 ? BoundParameterData(Value())
					                                   : BoundParameterData(Value::INTEGER(context.status));
				} else if (lower == "elapsed_ms") {
					values[name] = BoundParameterData(Value::BIGINT(context.elapsed_ms));
				} else {
					// A middleware cannot draw a value from the HTTP request by naming
					// an arbitrary parameter. This avoids accidental secret exposure.
					SetMiddlewareFailure(context);
					return false;
				}
			}

			// Existing request deadlines win. When middleware is called outside a
			// server request, still impose the finite default selected in context.
			auto fallback_timeout_ms = context.timeout_ms > 0 ? context.timeout_ms : 30000;
			auto timeout_ms = QuackapiRemainingTimeoutMillis(fallback_timeout_ms);
			if (timeout_ms <= 0) {
				SetMiddlewareFailure(context, 408);
				return false;
			}
			QuackapiQueryDeadline deadline(connection, timeout_ms);
			auto result = prepared->Execute(values, false);
			if (result->HasError() || deadline.Expired()) {
				SetMiddlewareFailure(context, deadline.Expired() ? 408 : 500);
				return false;
			}

			bool saw_rows = false;
			bool contract_checked = false;
			idx_t allow_col = 0, status_col = 0, body_col = 0, header_name_col = 0, header_value_col = 0;
			idx_t row_count = 0;
			bool blocked = false;
			int32_t blocked_status = 403;
			string blocked_body = "{\"detail\":\"Request rejected by middleware\"}";
			vector<std::pair<string, string>> headers;

			while (true) {
				auto chunk = result->Fetch();
				if (!chunk || chunk->size() == 0) {
					break;
				}
				saw_rows = true;
				if (!contract_checked) {
					if (!GetMiddlewareResultColumns(*result, allow_col, status_col, body_col, header_name_col,
					                                header_value_col)) {
						SetMiddlewareFailure(context);
						return false;
					}
					contract_checked = true;
				}
				for (idx_t row = 0; row < chunk->size(); row++) {
					if (++row_count > QUACKAPI_MIDDLEWARE_MAX_RESULT_ROWS) {
						SetMiddlewareFailure(context);
						return false;
					}
					auto allow = chunk->GetValue(allow_col, row);
					if (allow.IsNull()) {
						SetMiddlewareFailure(context);
						return false;
					}
					auto status = status_col < result->names.size() ? chunk->GetValue(status_col, row) : Value();
					auto body = body_col < result->names.size() ? chunk->GetValue(body_col, row) : Value();
					if (allow.GetValue<bool>() && (!status.IsNull() || !body.IsNull())) {
						SetMiddlewareFailure(context);
						return false;
					}
					if (header_name_col < result->names.size()) {
						auto header_name = chunk->GetValue(header_name_col, row);
						auto header_value = chunk->GetValue(header_value_col, row);
						if (header_name.IsNull() != header_value.IsNull()) {
							SetMiddlewareFailure(context);
							return false;
						}
						if (!header_name.IsNull()) {
							auto name = header_name.ToString();
							auto value = header_value.ToString();
							if (!IsSafeResponseHeader(name, value)) {
								SetMiddlewareFailure(context);
								return false;
							}
							headers.emplace_back(std::move(name), std::move(value));
						}
					}
					if (!allow.GetValue<bool>()) {
						auto status_value = status.IsNull() ? 403 : status.GetValue<int32_t>();
						if (!IsValidHttpStatus(status_value)) {
							SetMiddlewareFailure(context);
							return false;
						}
						auto body_value =
						    body.IsNull() ? string("{\"detail\":\"Request rejected by middleware\"}") : body.ToString();
						if (body_value.size() > QUACKAPI_MIDDLEWARE_MAX_BODY_BYTES) {
							SetMiddlewareFailure(context);
							return false;
						}
						if (blocked && (blocked_status != status_value || blocked_body != body_value)) {
							SetMiddlewareFailure(context);
							return false;
						}
						blocked = true;
						blocked_status = status_value;
						blocked_body = std::move(body_value);
					}
				}
			}
			if (deadline.Expired()) {
				SetMiddlewareFailure(context, 408);
				return false;
			}
			// No rows intentionally means a no-op. This permits audit statements
			// that filter to no events without granting an implicit response edit.
			(void)saw_rows;
			context.response_headers.insert(context.response_headers.end(), headers.begin(), headers.end());
			if (blocked) {
				context.status = blocked_status;
				context.response_body = std::move(blocked_body);
				return false;
			}
		}
		return true;
	} catch (...) {
		SetMiddlewareFailure(context);
		return false;
	}
}

MiddlewareDdlParserExtension::MiddlewareDdlParserExtension() {
	parse_function = MiddlewareDdlParse;
	plan_function = MiddlewareDdlPlan;
}

TableFunction GetApplyMiddlewareFunction() {
	return MakeApplyMiddlewareFunction();
}

TableFunction GetQuackapiMiddlewaresFunction() {
	return TableFunction("quackapi_middlewares", {}, MiddlewaresExec, MiddlewaresBind, MiddlewaresInit);
}

} // namespace duckdb
