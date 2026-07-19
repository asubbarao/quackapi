#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/parser/parser_extension.hpp"

#include "quackapi_ddl.hpp"
#include "quackapi_state.hpp"

namespace duckdb {

namespace {

string Trim(const string &input) {
	idx_t begin = 0;
	idx_t end = input.size();
	while (begin < end && StringUtil::CharacterIsSpace(input[begin])) {
		begin++;
	}
	while (end > begin && (StringUtil::CharacterIsSpace(input[end - 1]) || input[end - 1] == ';')) {
		end--;
	}
	return input.substr(begin, end - begin);
}

//! Parsed CREATE/DROP ROUTE statement, carried from parse to plan.
struct RouteDdlParseData : public ParserExtensionParseData {
	string action; // CREATE / DROP
	bool or_replace = false;
	QuackapiRoute route;

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto copy = make_uniq<RouteDdlParseData>();
		copy->action = action;
		copy->or_replace = or_replace;
		copy->route = route;
		return std::move(copy);
	}
	string ToString() const override {
		return action + " ROUTE " + route.name;
	}
};

bool IsHttpMethod(const string &method) {
	return method == "GET" || method == "POST" || method == "PUT" || method == "DELETE" || method == "PATCH" ||
	       method == "HEAD";
}

bool IsParamTypeName(const string &tok) {
	auto u = StringUtil::Upper(tok);
	return u == "INTEGER" || u == "INT" || u == "BIGINT" || u == "SMALLINT" || u == "TINYINT" || u == "HUGEINT" ||
	       u == "UBIGINT" || u == "UINTEGER" || u == "USMALLINT" || u == "UTINYINT" || u == "DOUBLE" || u == "FLOAT" ||
	       u == "REAL" || u == "DECIMAL" || u == "NUMERIC" || u == "VARCHAR" || u == "TEXT" || u == "STRING" ||
	       u == "BOOLEAN" || u == "BOOL";
}

//! Serialize param specs for the apply_route table function (plan → exec).
//! Format fields (FS=\x1f): name, type, has_def, def_null, def_raw, has_ge, ge, has_gt, gt,
//! has_le, le, has_lt, lt, has_min, min, has_max, max, source, external_name
//! Multiple specs separated by RS (\x1e). Text fields percent-encoded for RS/FS/%.
string EncodeField(const string &s) {
	string out;
	out.reserve(s.size());
	for (unsigned char c : s) {
		if (c == '%' || c == '\x1e' || c == '\x1f') {
			char buf[4];
			snprintf(buf, sizeof(buf), "%%%02X", c);
			out += buf;
		} else {
			out += static_cast<char>(c);
		}
	}
	return out;
}

string DecodeField(const string &s) {
	string out;
	out.reserve(s.size());
	for (idx_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size()) {
			unsigned int v = 0;
			if (sscanf(s.c_str() + i + 1, "%02x", &v) == 1 || sscanf(s.c_str() + i + 1, "%02X", &v) == 1) {
				out += static_cast<char>(v);
				i += 2;
				continue;
			}
		}
		out += s[i];
	}
	return out;
}

string ParamSourceToString(QuackapiParamSource src) {
	switch (src) {
	case QuackapiParamSource::HEADER:
		return "header";
	case QuackapiParamSource::COOKIE:
		return "cookie";
	case QuackapiParamSource::QUERY:
	default:
		return "query";
	}
}

QuackapiParamSource ParamSourceFromString(const string &s) {
	auto u = StringUtil::Lower(s);
	if (u == "header") {
		return QuackapiParamSource::HEADER;
	}
	if (u == "cookie") {
		return QuackapiParamSource::COOKIE;
	}
	return QuackapiParamSource::QUERY;
}

string SerializeParamSpecs(const vector<QuackapiParamSpec> &specs) {
	string result;
	for (idx_t i = 0; i < specs.size(); i++) {
		if (i > 0) {
			result += '\x1e';
		}
		auto &s = specs[i];
		result += EncodeField(s.name);
		result += '\x1f';
		result += EncodeField(s.type_name);
		result += '\x1f';
		result += s.has_default ? "1" : "0";
		result += '\x1f';
		result += s.default_is_null ? "1" : "0";
		result += '\x1f';
		result += EncodeField(s.default_raw);
		result += '\x1f';
		result += s.has_ge ? "1" : "0";
		result += '\x1f';
		result += std::to_string(s.ge);
		result += '\x1f';
		result += s.has_gt ? "1" : "0";
		result += '\x1f';
		result += std::to_string(s.gt);
		result += '\x1f';
		result += s.has_le ? "1" : "0";
		result += '\x1f';
		result += std::to_string(s.le);
		result += '\x1f';
		result += s.has_lt ? "1" : "0";
		result += '\x1f';
		result += std::to_string(s.lt);
		result += '\x1f';
		result += s.has_min_length ? "1" : "0";
		result += '\x1f';
		result += std::to_string(s.min_length);
		result += '\x1f';
		result += s.has_max_length ? "1" : "0";
		result += '\x1f';
		result += std::to_string(s.max_length);
		result += '\x1f';
		result += ParamSourceToString(s.source);
		result += '\x1f';
		result += EncodeField(s.external_name);
	}
	return result;
}

vector<QuackapiParamSpec> DeserializeParamSpecs(const string &blob) {
	vector<QuackapiParamSpec> specs;
	if (blob.empty()) {
		return specs;
	}
	// char Split preserves empty fields (string Split drops them).
	auto entries = StringUtil::Split(blob, '\x1e');
	for (auto &entry : entries) {
		if (entry.empty()) {
			continue;
		}
		auto fields = StringUtil::Split(entry, '\x1f');
		// Pad so we never index OOB on partial data (17 legacy + source + external).
		while (fields.size() < 19) {
			fields.push_back("");
		}
		QuackapiParamSpec s;
		s.name = DecodeField(fields[0]);
		s.type_name = DecodeField(fields[1]);
		s.has_default = fields[2] == "1";
		s.default_is_null = fields[3] == "1";
		s.default_raw = DecodeField(fields[4]);
		s.has_ge = fields[5] == "1";
		s.ge = atof(fields[6].c_str());
		s.has_gt = fields[7] == "1";
		s.gt = atof(fields[8].c_str());
		s.has_le = fields[9] == "1";
		s.le = atof(fields[10].c_str());
		s.has_lt = fields[11] == "1";
		s.lt = atof(fields[12].c_str());
		s.has_min_length = fields[13] == "1";
		s.min_length = (idx_t)atoll(fields[14].c_str());
		s.has_max_length = fields[15] == "1";
		s.max_length = (idx_t)atoll(fields[16].c_str());
		s.source = ParamSourceFromString(fields[17]);
		s.external_name = DecodeField(fields[18]);
		if (!s.name.empty()) {
			specs.push_back(std::move(s));
		}
	}
	return specs;
}

//! Grammar:
//!   CREATE [OR REPLACE] ROUTE <name> <METHOD> '<pattern>'
//!     [STATUS <n>] [REQUIRE <auth>]
//!     [BODY SCHEMA '<json-schema>']
//!     [PARAM <name> [<type>] [HEADER|COOKIE [wire-name]]
//!              [DEFAULT <lit>] [GE/GT/LE/LT/MIN_LENGTH/MAX_LENGTH <n>] ... ]
//!     AS <select>
//!   DROP ROUTE <name>
//!
//! HEADER binds from a request header (FastAPI Header). Default wire name is
//! the param name with underscores converted to hyphens (x_token → x-token).
//! COOKIE binds from the Cookie header (FastAPI Cookie); default wire name is
//! the param name as-is.
ParserExtensionParseResult RouteDdlParse(ParserExtensionInfo *, const string &query) {
	auto q = Trim(query);
	auto upper = StringUtil::Upper(q);

	bool or_replace = false;
	idx_t pos;
	if (StringUtil::StartsWith(upper, "CREATE ROUTE ")) {
		pos = 13;
	} else if (StringUtil::StartsWith(upper, "CREATE OR REPLACE ROUTE ")) {
		pos = 24;
		or_replace = true;
	} else if (StringUtil::StartsWith(upper, "DROP ROUTE ")) {
		auto name = Trim(q.substr(11));
		if (name.empty() || name.find(' ') != string::npos) {
			return ParserExtensionParseResult("DROP ROUTE expects a single route name");
		}
		auto data = make_uniq<RouteDdlParseData>();
		data->action = "DROP";
		data->route.name = name;
		return ParserExtensionParseResult(std::move(data));
	} else {
		// Not ours — let the core parser produce its own error.
		return ParserExtensionParseResult();
	}

	auto rest = Trim(q.substr(pos));

	// <name> <METHOD>
	auto first_space = rest.find(' ');
	if (first_space == string::npos) {
		return ParserExtensionParseResult("CREATE ROUTE <name> <METHOD> '<pattern>' AS <select>");
	}
	auto name = rest.substr(0, first_space);
	rest = Trim(rest.substr(first_space));
	auto second_space = rest.find(' ');
	if (second_space == string::npos) {
		return ParserExtensionParseResult("Expected <METHOD> '<pattern>' after route name");
	}
	auto method = StringUtil::Upper(rest.substr(0, second_space));
	if (!IsHttpMethod(method)) {
		return ParserExtensionParseResult("Unknown HTTP method \"" + method +
		                                  "\" — expected GET, POST, PUT, DELETE, PATCH or HEAD");
	}
	rest = Trim(rest.substr(second_space));

	// '<pattern>'
	if (rest.empty() || rest[0] != '\'') {
		return ParserExtensionParseResult("Expected quoted '<pattern>' after method");
	}
	auto pattern_end = rest.find('\'', 1);
	if (pattern_end == string::npos) {
		return ParserExtensionParseResult("Unterminated route pattern");
	}
	auto pattern = rest.substr(1, pattern_end - 1);
	if (pattern.empty() || pattern[0] != '/') {
		return ParserExtensionParseResult("Route pattern must start with '/'");
	}
	rest = Trim(rest.substr(pattern_end + 1));

	// Token boundary: first run of non-whitespace (spaces/tabs/newlines all OK).
	auto NextTokenEnd = [](const string &s) -> idx_t {
		idx_t i = 0;
		while (i < s.size() && !StringUtil::CharacterIsSpace(s[i])) {
			i++;
		}
		return i;
	};

	// [STATUS <n>]
	int status = 200;
	auto rest_upper = StringUtil::Upper(rest);
	if (StringUtil::StartsWith(rest_upper, "STATUS") &&
	    (rest.size() == 6 || StringUtil::CharacterIsSpace(rest[6]))) {
		rest = Trim(rest.substr(6));
		auto token_end = NextTokenEnd(rest);
		if (token_end == 0) {
			return ParserExtensionParseResult("Expected AS <select> after STATUS <n>");
		}
		status = atoi(rest.substr(0, token_end).c_str());
		if (status < 100 || status > 599) {
			return ParserExtensionParseResult("STATUS must be a valid HTTP status code");
		}
		rest = Trim(rest.substr(token_end));
		rest_upper = StringUtil::Upper(rest);
	}

	// [REQUIRE <auth-name>]
	string require_auth;
	if (StringUtil::StartsWith(rest_upper, "REQUIRE") &&
	    (rest.size() == 7 || StringUtil::CharacterIsSpace(rest[7]))) {
		rest = Trim(rest.substr(7));
		auto token_end = NextTokenEnd(rest);
		if (token_end == 0) {
			return ParserExtensionParseResult("Expected AS <select> after REQUIRE <auth>");
		}
		require_auth = rest.substr(0, token_end);
		if (require_auth.empty()) {
			return ParserExtensionParseResult("REQUIRE expects an auth name");
		}
		rest = Trim(rest.substr(token_end));
		rest_upper = StringUtil::Upper(rest);
	}

	// [BODY SCHEMA '<json-schema>'] — may appear before PARAM or after PARAM blocks.
	// Returns false and sets err on syntax error; true when clause absent or consumed.
	auto TryConsumeBodySchema = [&](string &err_out) -> bool {
		if (!(StringUtil::StartsWith(rest_upper, "BODY") && rest.size() > 4 &&
		      StringUtil::CharacterIsSpace(rest[4]))) {
			return true; // not present
		}
		string after_body = Trim(rest.substr(4));
		auto after_upper = StringUtil::Upper(after_body);
		if (!StringUtil::StartsWith(after_upper, "SCHEMA")) {
			err_out = "Expected BODY SCHEMA '<json-schema>'";
			return false;
		}
		if (after_body.size() == 6) {
			err_out = "BODY SCHEMA expects a quoted JSON schema string";
			return false;
		}
		string after_schema;
		if (StringUtil::CharacterIsSpace(after_body[6])) {
			after_schema = Trim(after_body.substr(6));
		} else if (after_body[6] == '\'') {
			after_schema = after_body.substr(6);
		} else {
			err_out = "Expected BODY SCHEMA '<json-schema>'";
			return false;
		}
		if (after_schema.empty() || after_schema[0] != '\'') {
			err_out = "BODY SCHEMA expects a quoted JSON schema string";
			return false;
		}
		// Quoted string with SQL '' escape.
		string schema;
		idx_t i = 1;
		while (i < after_schema.size()) {
			if (after_schema[i] == '\'') {
				if (i + 1 < after_schema.size() && after_schema[i + 1] == '\'') {
					schema += '\'';
					i += 2;
					continue;
				}
				break;
			}
			schema += after_schema[i];
			i++;
		}
		if (i >= after_schema.size() || after_schema[i] != '\'') {
			err_out = "Unterminated BODY SCHEMA string";
			return false;
		}
		// Assign via outer body_schema — declared below before this lambda is called.
		// (We reassign rest/rest_upper here; body_schema set by caller using schema.)
		rest = Trim(after_schema.substr(i + 1));
		rest_upper = StringUtil::Upper(rest);
		err_out = string("\x01") + schema; // success marker + payload
		return true;
	};

	string body_schema;
	{
		string bs_err;
		if (!TryConsumeBodySchema(bs_err)) {
			return ParserExtensionParseResult(bs_err);
		}
		if (!bs_err.empty() && bs_err[0] == '\x01') {
			body_schema = bs_err.substr(1);
		}
	}

	// Zero or more PARAM clauses (optional defaults + constraints).
	vector<QuackapiParamSpec> params;
	while (StringUtil::StartsWith(rest_upper, "PARAM") &&
	       (rest.size() == 5 || StringUtil::CharacterIsSpace(rest[5]))) {
		rest = Trim(rest.substr(5));
		if (rest.empty()) {
			return ParserExtensionParseResult("PARAM expects a parameter name");
		}
		QuackapiParamSpec spec;
		auto te = NextTokenEnd(rest);
		spec.name = rest.substr(0, te);
		rest = Trim(rest.substr(te));

		// optional type
		if (!rest.empty()) {
			te = NextTokenEnd(rest);
			auto tok = rest.substr(0, te);
			if (IsParamTypeName(tok)) {
				spec.type_name = StringUtil::Upper(tok);
				if (spec.type_name == "INT") {
					spec.type_name = "INTEGER";
				} else if (spec.type_name == "BOOL") {
					spec.type_name = "BOOLEAN";
				} else if (spec.type_name == "TEXT" || spec.type_name == "STRING") {
					spec.type_name = "VARCHAR";
				} else if (spec.type_name == "REAL") {
					spec.type_name = "FLOAT";
				}
				rest = Trim(rest.substr(te));
			}
		}

		// optional source: HEADER [wire-name] | COOKIE [wire-name]
		// May also appear after type or later among options.
		auto TryConsumeSource = [&]() -> bool {
			if (rest.empty()) {
				return false;
			}
			rest_upper = StringUtil::Upper(rest);
			te = NextTokenEnd(rest);
			auto key = StringUtil::Upper(rest.substr(0, te));
			if (key != "HEADER" && key != "COOKIE" && key != "QUERY") {
				return false;
			}
			if (key == "HEADER") {
				spec.source = QuackapiParamSource::HEADER;
			} else if (key == "COOKIE") {
				spec.source = QuackapiParamSource::COOKIE;
			} else {
				spec.source = QuackapiParamSource::QUERY;
			}
			rest = Trim(rest.substr(te));
			// Optional quoted or bare wire name (not a known option keyword).
			if (!rest.empty()) {
				if (rest[0] == '\'') {
					auto endq = rest.find('\'', 1);
					if (endq == string::npos) {
						return true; // leave rest; outer will error later if needed
					}
					spec.external_name = rest.substr(1, endq - 1);
					rest = Trim(rest.substr(endq + 1));
				} else {
					te = NextTokenEnd(rest);
					auto maybe = rest.substr(0, te);
					auto mu = StringUtil::Upper(maybe);
					if (mu != "DEFAULT" && mu != "GE" && mu != "GT" && mu != "LE" && mu != "LT" &&
					    mu != "MIN_LENGTH" && mu != "MAX_LENGTH" && mu != "PARAM" && mu != "BODY" &&
					    mu != "AS" && mu != "HEADER" && mu != "COOKIE" && mu != "QUERY" &&
					    !IsParamTypeName(maybe)) {
						spec.external_name = maybe;
						rest = Trim(rest.substr(te));
					}
				}
			}
			return true;
		};
		TryConsumeSource();

		// DEFAULT / GE / GT / LE / LT / MIN_LENGTH / MAX_LENGTH / HEADER / COOKIE
		while (!rest.empty()) {
			rest_upper = StringUtil::Upper(rest);
			if (StringUtil::StartsWith(rest_upper, "PARAM") &&
			    (rest.size() == 5 || StringUtil::CharacterIsSpace(rest[5]))) {
				break;
			}
			if (StringUtil::StartsWith(rest_upper, "BODY") && rest.size() > 4 &&
			    StringUtil::CharacterIsSpace(rest[4])) {
				break;
			}
			if (StringUtil::StartsWith(rest_upper, "AS") && rest.size() > 2 &&
			    StringUtil::CharacterIsSpace(rest[2])) {
				break;
			}
			// HEADER / COOKIE may appear after DEFAULT/constraints too.
			if (TryConsumeSource()) {
				continue;
			}
			te = NextTokenEnd(rest);
			auto key = StringUtil::Upper(rest.substr(0, te));
			string after_key = Trim(rest.substr(te));

			if (key == "DEFAULT") {
				if (after_key.empty()) {
					return ParserExtensionParseResult("PARAM DEFAULT expects a literal or NULL");
				}
				spec.has_default = true;
				if (after_key[0] == '\'') {
					auto endq = after_key.find('\'', 1);
					if (endq == string::npos) {
						return ParserExtensionParseResult("Unterminated DEFAULT string");
					}
					spec.default_raw = after_key.substr(1, endq - 1);
					spec.default_is_null = false;
					rest = Trim(after_key.substr(endq + 1));
				} else {
					auto lit_end = NextTokenEnd(after_key);
					auto lit = after_key.substr(0, lit_end);
					rest = Trim(after_key.substr(lit_end));
					if (StringUtil::Upper(lit) == "NULL") {
						spec.default_is_null = true;
						spec.default_raw.clear();
					} else {
						spec.default_is_null = false;
						spec.default_raw = lit;
					}
				}
				continue;
			}
			if (key == "GE" || key == "GT" || key == "LE" || key == "LT" || key == "MIN_LENGTH" ||
			    key == "MAX_LENGTH") {
				if (after_key.empty()) {
					return ParserExtensionParseResult("PARAM " + key + " expects a number");
				}
				auto num_end = NextTokenEnd(after_key);
				auto num = after_key.substr(0, num_end);
				rest = Trim(after_key.substr(num_end));
				if (key == "GE") {
					spec.has_ge = true;
					spec.ge = atof(num.c_str());
				} else if (key == "GT") {
					spec.has_gt = true;
					spec.gt = atof(num.c_str());
				} else if (key == "LE") {
					spec.has_le = true;
					spec.le = atof(num.c_str());
				} else if (key == "LT") {
					spec.has_lt = true;
					spec.lt = atof(num.c_str());
				} else if (key == "MIN_LENGTH") {
					spec.has_min_length = true;
					spec.min_length = (idx_t)atoll(num.c_str());
				} else {
					spec.has_max_length = true;
					spec.max_length = (idx_t)atoll(num.c_str());
				}
				continue;
			}
			return ParserExtensionParseResult(
			    "Unknown PARAM option \"" + key +
			    "\" — expected HEADER, COOKIE, DEFAULT, GE, GT, LE, LT, MIN_LENGTH, MAX_LENGTH");
		}

		params.push_back(std::move(spec));
		rest_upper = StringUtil::Upper(rest);
	}

	// BODY SCHEMA after PARAM blocks (if not already set)
	if (body_schema.empty()) {
		string bs_err;
		if (!TryConsumeBodySchema(bs_err)) {
			return ParserExtensionParseResult(bs_err);
		}
		if (!bs_err.empty() && bs_err[0] == '\x01') {
			body_schema = bs_err.substr(1);
		}
	}

	// AS <select> — any whitespace (spaces/tabs/newlines) after AS is accepted.
	// "AS SELECT …" and "AS\nSELECT …" are both valid; bare "AS" is not.
	if (!(StringUtil::StartsWith(rest_upper, "AS") && rest.size() > 2 &&
	      StringUtil::CharacterIsSpace(rest[2]))) {
		return ParserExtensionParseResult("Expected AS <select> in CREATE ROUTE");
	}
	auto handler = Trim(rest.substr(2));
	if (handler.empty()) {
		return ParserExtensionParseResult("Empty handler after AS");
	}

	auto data = make_uniq<RouteDdlParseData>();
	data->action = "CREATE";
	data->or_replace = or_replace;
	data->route.name = name;
	data->route.method = method;
	data->route.pattern = pattern;
	data->route.handler_sql = handler;
	data->route.status = status;
	data->route.require_auth = require_auth;
	data->route.params = std::move(params);
	data->route.body_schema = std::move(body_schema);
	return ParserExtensionParseResult(std::move(data));
}

//! Execution target for the planned DDL. All side effects happen here, at
//! execution time — plan/bind must not touch the registry (or run SQL: the
//! binder holds the ClientContext lock).
struct ApplyRouteBindData : public TableFunctionData {
	string action;
	bool or_replace = false;
	QuackapiRoute route;
	bool finished = false;
};

unique_ptr<FunctionData> ApplyRouteBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ApplyRouteBindData>();
	bind_data->action = input.inputs[0].GetValue<string>();
	bind_data->or_replace = input.inputs[1].GetValue<bool>();
	bind_data->route.name = input.inputs[2].GetValue<string>();
	bind_data->route.method = input.inputs[3].GetValue<string>();
	bind_data->route.pattern = input.inputs[4].GetValue<string>();
	bind_data->route.handler_sql = input.inputs[5].GetValue<string>();
	bind_data->route.status = input.inputs[6].GetValue<int32_t>();
	bind_data->route.require_auth = input.inputs[7].GetValue<string>();
	if (input.inputs.size() > 8 && !input.inputs[8].IsNull()) {
		bind_data->route.params = DeserializeParamSpecs(input.inputs[8].GetValue<string>());
	}
	if (input.inputs.size() > 9 && !input.inputs[9].IsNull()) {
		bind_data->route.body_schema = input.inputs[9].GetValue<string>();
	}
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");
	return std::move(bind_data);
}

void ApplyRouteExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ApplyRouteBindData>();
	if (bind_data.finished) {
		return;
	}
	auto &state = QuackapiState::Get(*context.db);
	string message;
	if (bind_data.action == "CREATE") {
		// Validate the handler SQL now so a broken route fails at CREATE time,
		// not at first request. Do this BEFORE mutating the registry so
		// CREATE OR REPLACE does not leave a half-applied route on failure.
		{
			Connection con(*context.db);
			auto prepared = con.Prepare(bind_data.route.handler_sql);
			if (prepared->HasError()) {
				throw InvalidInputException("Invalid handler SQL for route \"%s\": %s", bind_data.route.name,
				                            prepared->GetError());
			}
		}
		state.AddRoute(bind_data.route, bind_data.or_replace);
		message = StringUtil::Format("Route %s: %s %s", bind_data.route.name, bind_data.route.method,
		                             bind_data.route.pattern);
	} else {
		if (state.DropRoute(bind_data.route.name)) {
			message = StringUtil::Format("Dropped route %s", bind_data.route.name);
		} else {
			throw InvalidInputException("Route \"%s\" does not exist", bind_data.route.name);
		}
	}
	output.SetValue(0, 0, Value(message));
	output.SetCardinality(1);
	bind_data.finished = true;
}

TableFunction MakeApplyRouteFunction() {
	TableFunction function("quackapi_apply_route",
	                       {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                        LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR,
	                        LogicalType::VARCHAR, LogicalType::VARCHAR},
	                       ApplyRouteExec, ApplyRouteBind);
	return function;
}

ParserExtensionPlanResult RouteDdlPlan(ParserExtensionInfo *, ClientContext &,
                                       unique_ptr<ParserExtensionParseData> parse_data) {
	auto &data = static_cast<RouteDdlParseData &>(*parse_data);
	ParserExtensionPlanResult result;
	result.function = MakeApplyRouteFunction();
	result.parameters.push_back(Value(data.action));
	result.parameters.push_back(Value::BOOLEAN(data.or_replace));
	result.parameters.push_back(Value(data.route.name));
	result.parameters.push_back(Value(data.route.method));
	result.parameters.push_back(Value(data.route.pattern));
	result.parameters.push_back(Value(data.route.handler_sql));
	result.parameters.push_back(Value::INTEGER(data.route.status));
	result.parameters.push_back(Value(data.route.require_auth));
	result.parameters.push_back(Value(SerializeParamSpecs(data.route.params)));
	result.parameters.push_back(Value(data.route.body_schema));
	result.requires_valid_transaction = false;
	result.return_type = StatementReturnType::QUERY_RESULT;
	return result;
}

} // namespace

RouteDdlParserExtension::RouteDdlParserExtension() {
	parse_function = RouteDdlParse;
	plan_function = RouteDdlPlan;
}

TableFunction GetApplyRouteFunction() {
	return MakeApplyRouteFunction();
}

} // namespace duckdb
