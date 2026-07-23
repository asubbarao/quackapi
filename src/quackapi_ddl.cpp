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
#include "quackapi_util.hpp"

namespace duckdb {

namespace {

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

//! Join group.prefix + route path once at CREATE time (flat registry at runtime).
//! prefix='/api/v1' + '/items' or 'items' → '/api/v1/items'. Rejects double-prefix.
string JoinGroupPrefix(const string &prefix, const string &path) {
	if (prefix.empty() || prefix[0] != '/') {
		throw InvalidInputException("Group prefix must start with '/'");
	}
	string p = prefix;
	while (p.size() > 1 && p.back() == '/') {
		p.pop_back();
	}
	string r = path;
	if (r.empty()) {
		return p.empty() ? string("/") : p;
	}
	if (r[0] != '/') {
		r = "/" + r;
	}
	if (r == p || StringUtil::StartsWith(r, p + "/")) {
		throw InvalidInputException(
		    "Route path \"%s\" already starts with group prefix \"%s\" — omit the prefix or drop GROUP", path, prefix);
	}
	return p + r;
}

//! Grammar:
//!   CREATE [OR REPLACE] ROUTE <name> <METHOD> '<pattern>'
//!     [STATUS <n>] [REQUIRE <auth>] [GROUP <name> | IN GROUP <name>]
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
//! GROUP expands prefix+auth at CREATE (APIRouter-style); pattern may be relative.
ParserExtensionParseResult RouteDdlParse(ParserExtensionInfo *, const string &query) {
	auto q = QuackapiTrim(query);
	auto upper = StringUtil::Upper(q);

	bool or_replace = false;
	idx_t pos;
	if (StringUtil::StartsWith(upper, "CREATE ROUTE ")) {
		pos = 13;
	} else if (StringUtil::StartsWith(upper, "CREATE OR REPLACE ROUTE ")) {
		pos = 24;
		or_replace = true;
	} else if (StringUtil::StartsWith(upper, "DROP ROUTE ")) {
		auto name = QuackapiTrim(q.substr(11));
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

	auto rest = QuackapiTrim(q.substr(pos));

	// <name> <METHOD>
	auto first_space = rest.find(' ');
	if (first_space == string::npos) {
		return ParserExtensionParseResult("CREATE ROUTE <name> <METHOD> '<pattern>' AS <select>");
	}
	auto name = rest.substr(0, first_space);
	rest = QuackapiTrim(rest.substr(first_space));
	auto second_space = rest.find(' ');
	if (second_space == string::npos) {
		return ParserExtensionParseResult("Expected <METHOD> '<pattern>' after route name");
	}
	auto method = StringUtil::Upper(rest.substr(0, second_space));
	if (!IsHttpMethod(method)) {
		return ParserExtensionParseResult("Unknown HTTP method \"" + method +
		                                  "\" — expected GET, POST, PUT, DELETE, PATCH or HEAD");
	}
	rest = QuackapiTrim(rest.substr(second_space));

	// '<pattern>' — ungrouped routes must start with '/'; GROUP may use relative paths.
	if (rest.empty() || rest[0] != '\'') {
		return ParserExtensionParseResult("Expected quoted '<pattern>' after method");
	}
	auto pattern_end = rest.find('\'', 1);
	if (pattern_end == string::npos) {
		return ParserExtensionParseResult("Unterminated route pattern");
	}
	auto pattern = rest.substr(1, pattern_end - 1);
	if (pattern.empty()) {
		return ParserExtensionParseResult("Route pattern must not be empty");
	}
	rest = QuackapiTrim(rest.substr(pattern_end + 1));

	// Token boundary: first run of non-whitespace (spaces/tabs/newlines all OK).
	auto NextTokenEnd = [](const string &s) -> idx_t {
		idx_t i = 0;
		while (i < s.size() && !StringUtil::CharacterIsSpace(s[i])) {
			i++;
		}
		return i;
	};

	// Optional clauses in any order: STATUS / REQUIRE / GROUP|IN GROUP
	int status = 200;
	string require_auth;
	string group_name;
	auto rest_upper = StringUtil::Upper(rest);
	for (int clause_round = 0; clause_round < 6; clause_round++) {
		rest_upper = StringUtil::Upper(rest);
		// [STATUS <n>]
		if (StringUtil::StartsWith(rest_upper, "STATUS") &&
		    (rest.size() == 6 || StringUtil::CharacterIsSpace(rest[6]))) {
			rest = QuackapiTrim(rest.substr(6));
			auto token_end = NextTokenEnd(rest);
			if (token_end == 0) {
				return ParserExtensionParseResult("Expected AS <select> after STATUS <n>");
			}
			status = atoi(rest.substr(0, token_end).c_str());
			if (status < 100 || status > 599) {
				return ParserExtensionParseResult("STATUS must be a valid HTTP status code");
			}
			rest = QuackapiTrim(rest.substr(token_end));
			continue;
		}
		// [REQUIRE <auth-name>]
		if (StringUtil::StartsWith(rest_upper, "REQUIRE") &&
		    (rest.size() == 7 || StringUtil::CharacterIsSpace(rest[7]))) {
			rest = QuackapiTrim(rest.substr(7));
			auto token_end = NextTokenEnd(rest);
			if (token_end == 0) {
				return ParserExtensionParseResult("Expected AS <select> after REQUIRE <auth>");
			}
			require_auth = rest.substr(0, token_end);
			if (require_auth.empty()) {
				return ParserExtensionParseResult("REQUIRE expects an auth name");
			}
			rest = QuackapiTrim(rest.substr(token_end));
			continue;
		}
		// [IN GROUP <name>] or [GROUP <name>]
		if (StringUtil::StartsWith(rest_upper, "IN") && rest.size() > 2 && StringUtil::CharacterIsSpace(rest[2])) {
			string after_in = QuackapiTrim(rest.substr(2));
			auto after_upper = StringUtil::Upper(after_in);
			if (StringUtil::StartsWith(after_upper, "GROUP") &&
			    (after_in.size() == 5 || StringUtil::CharacterIsSpace(after_in[5]))) {
				after_in = QuackapiTrim(after_in.substr(5));
				auto token_end = NextTokenEnd(after_in);
				if (token_end == 0) {
					return ParserExtensionParseResult("IN GROUP expects a group name");
				}
				if (!group_name.empty()) {
					return ParserExtensionParseResult("GROUP specified more than once");
				}
				group_name = after_in.substr(0, token_end);
				rest = QuackapiTrim(after_in.substr(token_end));
				continue;
			}
		}
		if (StringUtil::StartsWith(rest_upper, "GROUP") &&
		    (rest.size() == 5 || StringUtil::CharacterIsSpace(rest[5]))) {
			rest = QuackapiTrim(rest.substr(5));
			auto token_end = NextTokenEnd(rest);
			if (token_end == 0) {
				return ParserExtensionParseResult("GROUP expects a group name");
			}
			if (!group_name.empty()) {
				return ParserExtensionParseResult("GROUP specified more than once");
			}
			group_name = rest.substr(0, token_end);
			rest = QuackapiTrim(rest.substr(token_end));
			continue;
		}
		break;
	}
	rest_upper = StringUtil::Upper(rest);

	// Ungrouped routes still require an absolute path starting with '/'.
	if (group_name.empty() && pattern[0] != '/') {
		return ParserExtensionParseResult("Route pattern must start with '/'");
	}

	// [BODY SCHEMA '<json-schema>'] — may appear before PARAM or after PARAM blocks.
	// Returns false and sets err on syntax error; true when clause absent or consumed.
	auto TryConsumeBodySchema = [&](string &err_out) -> bool {
		if (!(StringUtil::StartsWith(rest_upper, "BODY") && rest.size() > 4 && StringUtil::CharacterIsSpace(rest[4]))) {
			return true; // not present
		}
		string after_body = QuackapiTrim(rest.substr(4));
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
			after_schema = QuackapiTrim(after_body.substr(6));
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
		rest = QuackapiTrim(after_schema.substr(i + 1));
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
	while (StringUtil::StartsWith(rest_upper, "PARAM") && (rest.size() == 5 || StringUtil::CharacterIsSpace(rest[5]))) {
		rest = QuackapiTrim(rest.substr(5));
		if (rest.empty()) {
			return ParserExtensionParseResult("PARAM expects a parameter name");
		}
		QuackapiParamSpec spec;
		auto te = NextTokenEnd(rest);
		spec.name = rest.substr(0, te);
		rest = QuackapiTrim(rest.substr(te));

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
				rest = QuackapiTrim(rest.substr(te));
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
			rest = QuackapiTrim(rest.substr(te));
			// Optional quoted or bare wire name (not a known option keyword).
			if (!rest.empty()) {
				if (rest[0] == '\'') {
					auto endq = rest.find('\'', 1);
					if (endq == string::npos) {
						return true; // leave rest; outer will error later if needed
					}
					spec.external_name = rest.substr(1, endq - 1);
					rest = QuackapiTrim(rest.substr(endq + 1));
				} else {
					te = NextTokenEnd(rest);
					auto maybe = rest.substr(0, te);
					auto mu = StringUtil::Upper(maybe);
					if (mu != "DEFAULT" && mu != "GE" && mu != "GT" && mu != "LE" && mu != "LT" && mu != "MIN_LENGTH" &&
					    mu != "MAX_LENGTH" && mu != "PARAM" && mu != "BODY" && mu != "AS" && mu != "HEADER" &&
					    mu != "COOKIE" && mu != "QUERY" && !IsParamTypeName(maybe)) {
						spec.external_name = maybe;
						rest = QuackapiTrim(rest.substr(te));
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
			if (StringUtil::StartsWith(rest_upper, "AS") && rest.size() > 2 && StringUtil::CharacterIsSpace(rest[2])) {
				break;
			}
			// HEADER / COOKIE may appear after DEFAULT/constraints too.
			if (TryConsumeSource()) {
				continue;
			}
			te = NextTokenEnd(rest);
			auto key = StringUtil::Upper(rest.substr(0, te));
			string after_key = QuackapiTrim(rest.substr(te));

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
					rest = QuackapiTrim(after_key.substr(endq + 1));
				} else {
					auto lit_end = NextTokenEnd(after_key);
					auto lit = after_key.substr(0, lit_end);
					rest = QuackapiTrim(after_key.substr(lit_end));
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
				rest = QuackapiTrim(after_key.substr(num_end));
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
	if (!(StringUtil::StartsWith(rest_upper, "AS") && rest.size() > 2 && StringUtil::CharacterIsSpace(rest[2]))) {
		return ParserExtensionParseResult("Expected AS <select> in CREATE ROUTE");
	}
	auto handler = QuackapiTrim(rest.substr(2));
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
	data->route.group_name = group_name;
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
	if (input.inputs.size() > 10 && !input.inputs[10].IsNull()) {
		bind_data->route.group_name = input.inputs[10].GetValue<string>();
	}
	BindStatusColumn(return_types, names);
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
		// Expand GROUP prefix / default auth / tags before registry insert.
		// Runtime MatchPattern stays absolute — no request-time rewrite.
		if (!bind_data.route.group_name.empty()) {
			QuackapiGroup group;
			if (!state.GetGroup(bind_data.route.group_name, group)) {
				throw InvalidInputException("Group \"%s\" does not exist", bind_data.route.group_name);
			}
			bind_data.route.pattern = JoinGroupPrefix(group.prefix, bind_data.route.pattern);
			if (bind_data.route.require_auth.empty() && !group.require_auth.empty()) {
				bind_data.route.require_auth = group.require_auth;
			}
			if (bind_data.route.tags.empty() && !group.tags.empty()) {
				bind_data.route.tags = group.tags;
			}
			// policy seam: group.policy reserved for future shared policy; unused in v1.
		} else if (bind_data.route.pattern.empty() || bind_data.route.pattern[0] != '/') {
			throw InvalidInputException("Route pattern must start with '/'");
		}
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
	EmitOneShotStatus(output, bind_data.finished, message);
}

TableFunction MakeApplyRouteFunction() {
	return MakeApplyDdlFunction("quackapi_apply_route",
	                            {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                             LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR,
	                             LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                            ApplyRouteExec, ApplyRouteBind);
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
	result.parameters.push_back(Value(data.route.group_name));
	FinishDdlPlan(result);
	return result;
}

//===--------------------------------------------------------------------===//
// CREATE [OR REPLACE] GROUP / DROP GROUP
//===--------------------------------------------------------------------===//

struct GroupDdlParseData : public ParserExtensionParseData {
	string action; // CREATE / DROP
	bool or_replace = false;
	QuackapiGroup group;

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto copy = make_uniq<GroupDdlParseData>();
		copy->action = action;
		copy->or_replace = or_replace;
		copy->group = group;
		return std::move(copy);
	}
	string ToString() const override {
		return action + " GROUP " + group.name;
	}
};

//! Parse a single quoted SQL string starting at s[0]=='\''; advances *end past close.
bool ParseQuotedString(const string &s, idx_t start, string &out, idx_t &end) {
	if (start >= s.size() || s[start] != '\'') {
		return false;
	}
	string result;
	idx_t i = start + 1;
	while (i < s.size()) {
		if (s[i] == '\'') {
			if (i + 1 < s.size() && s[i + 1] == '\'') {
				result += '\'';
				i += 2;
				continue;
			}
			out = result;
			end = i + 1;
			return true;
		}
		result += s[i];
		i++;
	}
	return false;
}

//! Grammar:
//!   CREATE [OR REPLACE] [API] GROUP <name> WITH (
//!     prefix='/abs', [auth=<name>|require=<name>], [tags='csv'], [policy=<name>]
//!   )
//!   DROP [API] GROUP <name>
//!
//! Also accepts positional-ish keywords without WITH for SPEC compatibility:
//!   CREATE API GROUP <name> PREFIX '/p' [TAGS 't'] [REQUIRE <auth>]
ParserExtensionParseResult GroupDdlParse(ParserExtensionInfo *, const string &query) {
	auto q = QuackapiTrim(query);
	auto upper = StringUtil::Upper(q);

	bool or_replace = false;
	idx_t pos = 0;
	bool matched = false;

	// DROP forms
	if (StringUtil::StartsWith(upper, "DROP API GROUP ")) {
		auto name = QuackapiTrim(q.substr(15));
		if (name.empty() || name.find(' ') != string::npos) {
			return ParserExtensionParseResult("DROP API GROUP expects a single group name");
		}
		auto data = make_uniq<GroupDdlParseData>();
		data->action = "DROP";
		data->group.name = name;
		return ParserExtensionParseResult(std::move(data));
	}
	if (StringUtil::StartsWith(upper, "DROP GROUP ")) {
		auto name = QuackapiTrim(q.substr(11));
		if (name.empty() || name.find(' ') != string::npos) {
			return ParserExtensionParseResult("DROP GROUP expects a single group name");
		}
		auto data = make_uniq<GroupDdlParseData>();
		data->action = "DROP";
		data->group.name = name;
		return ParserExtensionParseResult(std::move(data));
	}

	// CREATE forms
	if (StringUtil::StartsWith(upper, "CREATE OR REPLACE API GROUP ")) {
		pos = 28;
		or_replace = true;
		matched = true;
	} else if (StringUtil::StartsWith(upper, "CREATE OR REPLACE GROUP ")) {
		pos = 24;
		or_replace = true;
		matched = true;
	} else if (StringUtil::StartsWith(upper, "CREATE API GROUP ")) {
		pos = 17;
		matched = true;
	} else if (StringUtil::StartsWith(upper, "CREATE GROUP ")) {
		pos = 13;
		matched = true;
	}
	if (!matched) {
		return ParserExtensionParseResult();
	}

	auto rest = QuackapiTrim(q.substr(pos));
	auto first_space = rest.find(' ');
	if (first_space == string::npos) {
		// bare name only is invalid
		if (rest.empty()) {
			return ParserExtensionParseResult("CREATE GROUP <name> WITH (prefix='...', ...)");
		}
		// name with no options
		return ParserExtensionParseResult("CREATE GROUP requires WITH (prefix='...') or PREFIX '...'");
	}
	// name may be followed by WITH or PREFIX
	string name;
	{
		// take first token as name
		idx_t i = 0;
		while (i < rest.size() && !StringUtil::CharacterIsSpace(rest[i])) {
			i++;
		}
		name = rest.substr(0, i);
		rest = QuackapiTrim(rest.substr(i));
	}
	if (name.empty()) {
		return ParserExtensionParseResult("CREATE GROUP expects a group name");
	}

	QuackapiGroup group;
	group.name = name;
	auto rest_upper = StringUtil::Upper(rest);

	auto NextTokenEnd = [](const string &s) -> idx_t {
		idx_t i = 0;
		while (i < s.size() && !StringUtil::CharacterIsSpace(s[i])) {
			i++;
		}
		return i;
	};

	if (StringUtil::StartsWith(rest_upper, "WITH")) {
		rest = QuackapiTrim(rest.substr(4));
		if (rest.empty() || rest[0] != '(') {
			return ParserExtensionParseResult("Expected WITH (prefix='...', ...)");
		}
		auto close = rest.rfind(')');
		if (close == string::npos) {
			return ParserExtensionParseResult("Unterminated WITH (...) options");
		}
		auto opts = QuackapiTrim(rest.substr(1, close - 1));
		rest = QuackapiTrim(rest.substr(close + 1));
		if (!rest.empty()) {
			return ParserExtensionParseResult("Unexpected tokens after GROUP WITH (...)");
		}
		// Parse comma-separated key=value
		idx_t oi = 0;
		bool have_prefix = false;
		while (oi < opts.size()) {
			while (oi < opts.size() && (StringUtil::CharacterIsSpace(opts[oi]) || opts[oi] == ',')) {
				oi++;
			}
			if (oi >= opts.size()) {
				break;
			}
			// key
			idx_t key_start = oi;
			while (oi < opts.size() && !StringUtil::CharacterIsSpace(opts[oi]) && opts[oi] != '=' && opts[oi] != ',') {
				oi++;
			}
			auto key = StringUtil::Lower(opts.substr(key_start, oi - key_start));
			while (oi < opts.size() && StringUtil::CharacterIsSpace(opts[oi])) {
				oi++;
			}
			if (oi >= opts.size() || opts[oi] != '=') {
				return ParserExtensionParseResult("GROUP option \"" + key + "\" expects =value");
			}
			oi++; // =
			while (oi < opts.size() && StringUtil::CharacterIsSpace(opts[oi])) {
				oi++;
			}
			if (oi >= opts.size()) {
				return ParserExtensionParseResult("GROUP option \"" + key + "\" expects a value");
			}
			string value;
			if (opts[oi] == '\'') {
				idx_t end = 0;
				if (!ParseQuotedString(opts, oi, value, end)) {
					return ParserExtensionParseResult("Unterminated quoted value for " + key);
				}
				oi = end;
			} else {
				idx_t vstart = oi;
				while (oi < opts.size() && opts[oi] != ',' && !StringUtil::CharacterIsSpace(opts[oi])) {
					oi++;
				}
				value = opts.substr(vstart, oi - vstart);
			}
			if (key == "prefix") {
				if (value.empty() || value[0] != '/') {
					return ParserExtensionParseResult("GROUP prefix must start with '/'");
				}
				group.prefix = value;
				have_prefix = true;
			} else if (key == "auth" || key == "require" || key == "require_auth") {
				group.require_auth = value;
			} else if (key == "tags") {
				group.tags = value;
			} else if (key == "policy") {
				// Seam for future shared policy; stored, unused at request time in v1.
				group.policy = value;
			} else {
				return ParserExtensionParseResult("Unknown GROUP option \"" + key +
				                                  "\" — expected prefix, auth, tags, policy");
			}
		}
		if (!have_prefix) {
			return ParserExtensionParseResult("CREATE GROUP requires prefix='/...'");
		}
	} else if (StringUtil::StartsWith(rest_upper, "PREFIX")) {
		// SPEC form: PREFIX '/p' [TAGS 't'] [REQUIRE auth]
		rest = QuackapiTrim(rest.substr(6));
		if (rest.empty() || rest[0] != '\'') {
			return ParserExtensionParseResult("PREFIX expects a quoted path");
		}
		idx_t end = 0;
		if (!ParseQuotedString(rest, 0, group.prefix, end)) {
			return ParserExtensionParseResult("Unterminated PREFIX string");
		}
		if (group.prefix.empty() || group.prefix[0] != '/') {
			return ParserExtensionParseResult("GROUP prefix must start with '/'");
		}
		rest = QuackapiTrim(rest.substr(end));
		while (!rest.empty()) {
			rest_upper = StringUtil::Upper(rest);
			if (StringUtil::StartsWith(rest_upper, "TAGS") &&
			    (rest.size() == 4 || StringUtil::CharacterIsSpace(rest[4]))) {
				rest = QuackapiTrim(rest.substr(4));
				if (rest.empty() || rest[0] != '\'') {
					return ParserExtensionParseResult("TAGS expects a quoted string");
				}
				idx_t tend = 0;
				if (!ParseQuotedString(rest, 0, group.tags, tend)) {
					return ParserExtensionParseResult("Unterminated TAGS string");
				}
				rest = QuackapiTrim(rest.substr(tend));
				continue;
			}
			if (StringUtil::StartsWith(rest_upper, "REQUIRE") &&
			    (rest.size() == 7 || StringUtil::CharacterIsSpace(rest[7]))) {
				rest = QuackapiTrim(rest.substr(7));
				auto te = NextTokenEnd(rest);
				if (te == 0) {
					return ParserExtensionParseResult("REQUIRE expects an auth name");
				}
				group.require_auth = rest.substr(0, te);
				rest = QuackapiTrim(rest.substr(te));
				continue;
			}
			return ParserExtensionParseResult("Unexpected token in CREATE GROUP — expected TAGS or REQUIRE");
		}
	} else {
		return ParserExtensionParseResult("CREATE GROUP requires WITH (prefix='...') or PREFIX '...'");
	}

	auto data = make_uniq<GroupDdlParseData>();
	data->action = "CREATE";
	data->or_replace = or_replace;
	data->group = group;
	return ParserExtensionParseResult(std::move(data));
}

struct ApplyGroupBindData : public TableFunctionData {
	string action;
	bool or_replace = false;
	QuackapiGroup group;
	bool finished = false;
};

unique_ptr<FunctionData> ApplyGroupBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ApplyGroupBindData>();
	bind_data->action = input.inputs[0].GetValue<string>();
	bind_data->or_replace = input.inputs[1].GetValue<bool>();
	bind_data->group.name = input.inputs[2].GetValue<string>();
	bind_data->group.prefix = input.inputs[3].GetValue<string>();
	bind_data->group.require_auth = input.inputs[4].GetValue<string>();
	bind_data->group.tags = input.inputs[5].GetValue<string>();
	bind_data->group.policy = input.inputs[6].GetValue<string>();
	BindStatusColumn(return_types, names);
	return std::move(bind_data);
}

void ApplyGroupExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ApplyGroupBindData>();
	if (bind_data.finished) {
		return;
	}
	auto &state = QuackapiState::Get(*context.db);
	string message;
	if (bind_data.action == "CREATE") {
		if (bind_data.group.prefix.empty() || bind_data.group.prefix[0] != '/') {
			throw InvalidInputException("Group prefix must start with '/'");
		}
		state.AddGroup(bind_data.group, bind_data.or_replace);
		message = StringUtil::Format("Group %s: %s", bind_data.group.name, bind_data.group.prefix);
	} else {
		if (state.DropGroup(bind_data.group.name)) {
			message = StringUtil::Format("Dropped group %s", bind_data.group.name);
		} else {
			throw InvalidInputException("Group \"%s\" does not exist", bind_data.group.name);
		}
	}
	EmitOneShotStatus(output, bind_data.finished, message);
}

TableFunction MakeApplyGroupFunction() {
	return MakeApplyDdlFunction("quackapi_apply_group",
	                            {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                             LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                            ApplyGroupExec, ApplyGroupBind);
}

ParserExtensionPlanResult GroupDdlPlan(ParserExtensionInfo *, ClientContext &,
                                       unique_ptr<ParserExtensionParseData> parse_data) {
	auto &data = static_cast<GroupDdlParseData &>(*parse_data);
	ParserExtensionPlanResult result;
	result.function = MakeApplyGroupFunction();
	result.parameters.push_back(Value(data.action));
	result.parameters.push_back(Value::BOOLEAN(data.or_replace));
	result.parameters.push_back(Value(data.group.name));
	result.parameters.push_back(Value(data.group.prefix));
	result.parameters.push_back(Value(data.group.require_auth));
	result.parameters.push_back(Value(data.group.tags));
	result.parameters.push_back(Value(data.group.policy));
	FinishDdlPlan(result);
	return result;
}

//===--------------------------------------------------------------------===//
// quackapi_groups() — name, prefix, require_auth, tags, members
//===--------------------------------------------------------------------===//

struct GroupsBindData : public TableFunctionData {};

struct GroupsGlobalState : public GlobalTableFunctionState {
	vector<QuackapiGroup> groups;
	vector<QuackapiRoute> routes;
	idx_t offset = 0;
};

unique_ptr<FunctionData> GroupsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                    vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("prefix");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("require_auth");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("members");
	return make_uniq<GroupsBindData>();
}

unique_ptr<GlobalTableFunctionState> GroupsInit(ClientContext &context, TableFunctionInitInput &) {
	auto state = make_uniq<GroupsGlobalState>();
	auto &qa = QuackapiState::Get(*context.db);
	state->groups = qa.SnapshotGroups();
	state->routes = qa.SnapshotRoutes();
	return std::move(state);
}

void GroupsExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<GroupsGlobalState>();
	idx_t row = 0;
	while (state.offset < state.groups.size() && row < STANDARD_VECTOR_SIZE) {
		auto &g = state.groups[state.offset];
		// members: comma-separated route names that joined this group
		string members;
		for (auto &r : state.routes) {
			if (r.group_name == g.name) {
				if (!members.empty()) {
					members += ",";
				}
				members += r.name;
			}
		}
		output.SetValue(0, row, Value(g.name));
		output.SetValue(1, row, Value(g.prefix));
		output.SetValue(2, row, Value(g.require_auth));
		output.SetValue(3, row, Value(g.tags));
		output.SetValue(4, row, Value(members));
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

} // namespace

RouteDdlParserExtension::RouteDdlParserExtension() {
	parse_function = RouteDdlParse;
	plan_function = RouteDdlPlan;
}

TableFunction GetApplyRouteFunction() {
	return MakeApplyRouteFunction();
}

GroupDdlParserExtension::GroupDdlParserExtension() {
	parse_function = GroupDdlParse;
	plan_function = GroupDdlPlan;
}

TableFunction GetApplyGroupFunction() {
	return MakeApplyGroupFunction();
}

TableFunction GetQuackapiGroupsFunction() {
	return TableFunction("quackapi_groups", {}, GroupsExec, GroupsBind, GroupsInit);
}

} // namespace duckdb
