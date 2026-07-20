#include "quackapi_server.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/main/query_result.hpp"

#include "quackapi_auth.hpp"
#include "quackapi_openapi.hpp"
#include "quackapi_policy.hpp"
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

//! FastAPI-shaped validation error body (loc = [kind, name]).
string ValidationErrorJson(const string &loc_kind, const string &param_name, const string &msg, const string &type) {
	return "{\"detail\":[{\"loc\":[\"" + JsonEscape(loc_kind) + "\",\"" + JsonEscape(param_name) + "\"],\"msg\":\"" +
	       JsonEscape(msg) + "\",\"type\":\"" + JsonEscape(type) + "\"}]}";
}

//! FastAPI-shaped body-only validation error (loc = ["body"]).
string ValidationErrorJsonBody(const string &msg, const string &type) {
	return "{\"detail\":[{\"loc\":[\"body\"],\"msg\":\"" + JsonEscape(msg) + "\",\"type\":\"" + JsonEscape(type) +
	       "\"}]}";
}

//! Media type from Content-Type (strip parameters; lowercased).
string ContentTypeMedia(const case_insensitive_map_t<string> &headers) {
	auto it = headers.find("Content-Type");
	if (it == headers.end()) {
		return string();
	}
	string ct = it->second;
	auto sc = ct.find(';');
	if (sc != string::npos) {
		ct = ct.substr(0, sc);
	}
	StringUtil::Trim(ct);
	return StringUtil::Lower(ct);
}

bool IsJsonMediaType(const string &media) {
	return media == "application/json" || StringUtil::EndsWith(media, "+json");
}

bool IsFormUrlEncodedMediaType(const string &media) {
	return media == "application/x-www-form-urlencoded";
}

bool IsMultipartMediaType(const string &media) {
	return StringUtil::StartsWith(media, "multipart/form-data");
}

bool IsBodyMethod(const string &method) {
	return method == "POST" || method == "PUT" || method == "PATCH";
}

//! application/x-www-form-urlencoded → key/value map (last wins).
void ParseFormUrlEncoded(const string &body, case_insensitive_map_t<string> &out) {
	idx_t i = 0;
	while (i < body.size()) {
		idx_t amp = body.find('&', i);
		if (amp == string::npos) {
			amp = body.size();
		}
		string pair = body.substr(i, amp - i);
		if (!pair.empty()) {
			auto eq = pair.find('=');
			string key, val;
			if (eq == string::npos) {
				key = duckdb_httplib::decode_query_component(pair, true);
			} else {
				key = duckdb_httplib::decode_query_component(pair.substr(0, eq), true);
				val = duckdb_httplib::decode_query_component(pair.substr(eq + 1), true);
			}
			if (!key.empty()) {
				out[key] = val;
			}
		}
		i = amp + 1;
	}
}

//! Extract top-level JSON object fields as string values for binding.
//! On invalid JSON sets err_json and returns false. Arrays/non-objects → model_attributes_type.
bool ExtractJsonBodyFields(Connection &con, const string &raw_body, case_insensitive_map_t<string> &fields,
                           string &err_json) {
	if (raw_body.empty()) {
		err_json = ValidationErrorJsonBody("JSON decode error", "json_invalid");
		return false;
	}
	// Validate JSON parse first (TRY_CAST → NULL on failure).
	auto check = con.Query("SELECT TRY_CAST(? AS JSON) IS NOT NULL", Value(raw_body));
	if (check->HasError()) {
		err_json = ValidationErrorJsonBody("JSON decode error", "json_invalid");
		return false;
	}
	auto check_chunk = check->Fetch();
	if (!check_chunk || check_chunk->size() == 0 || !check_chunk->GetValue(0, 0).GetValue<bool>()) {
		err_json = ValidationErrorJsonBody("JSON decode error", "json_invalid");
		return false;
	}
	// Must be an object to extract named fields (FastAPI body model).
	auto type_res = con.Query("SELECT json_type(?::JSON)", Value(raw_body));
	if (type_res->HasError()) {
		err_json = ValidationErrorJsonBody("JSON decode error", "json_invalid");
		return false;
	}
	auto type_chunk = type_res->Fetch();
	string jtype = type_chunk && type_chunk->size() > 0 && !type_chunk->GetValue(0, 0).IsNull()
	                   ? type_chunk->GetValue(0, 0).GetValue<string>()
	                   : string();
	if (jtype != "OBJECT") {
		err_json = ValidationErrorJsonBody(
		    "Input should be a valid dictionary or object to extract fields from", "model_attributes_type");
		return false;
	}
	// Flatten top-level scalars: key → string form for BindParamValue.
	auto fields_res = con.Query(
	    "SELECT k AS key, "
	    "  CASE json_type(json_extract(doc, '$.' || k)) "
	    "    WHEN 'VARCHAR' THEN json_extract_string(doc, '$.' || k) "
	    "    WHEN 'NULL' THEN NULL "
	    "    ELSE CAST(json_extract(doc, '$.' || k) AS VARCHAR) "
	    "  END AS val "
	    "FROM (SELECT ?::JSON AS doc) t, "
	    "     UNNEST(json_keys(doc)) AS u(k)",
	    Value(raw_body));
	if (fields_res->HasError()) {
		err_json = ValidationErrorJsonBody("JSON decode error", "json_invalid");
		return false;
	}
	while (true) {
		auto chunk = fields_res->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t row = 0; row < chunk->size(); row++) {
			auto key_v = chunk->GetValue(0, row);
			if (key_v.IsNull()) {
				continue;
			}
			string key = key_v.GetValue<string>();
			auto val_v = chunk->GetValue(1, row);
			if (val_v.IsNull()) {
				// Explicit JSON null: bind as empty string marker? Prefer skip so
				// missing-style required still 422, or bind "" and let cast fail.
				// FastAPI null for int → type error. Store special raw "null" text
				// only if we want that; store empty and rely on type check is weak.
				// Use the literal word that fails integer cast for ints: "null"
				fields[key] = "null";
				continue;
			}
			fields[key] = val_v.GetValue<string>();
		}
	}
	return true;
}

//! Validate body against BODY SCHEMA using community json_schema extension.
//! json_schema_validate returns true on pass and THROWS on fail — wrap with try().
bool ValidateBodySchema(Connection &con, const string &schema, const string &raw_body, string &err_json) {
	// LOAD is idempotent; INSTALL FROM community on first failure (network).
	auto load = con.Query("LOAD json_schema");
	if (load->HasError()) {
		auto inst = con.Query("INSTALL json_schema FROM community");
		if (!inst->HasError()) {
			load = con.Query("LOAD json_schema");
		}
		if (load->HasError()) {
			fprintf(stderr, "quackapi: json_schema unavailable: %s\n", load->GetError().c_str());
			err_json = ValidationErrorJsonBody("json_schema extension unavailable", "value_error");
			return false;
		}
	}
	// try() → true on pass, NULL when the function throws (never returns false).
	auto res = con.Query("SELECT try(json_schema_validate(?::JSON, ?::JSON))", Value(schema), Value(raw_body));
	if (res->HasError()) {
		fprintf(stderr, "quackapi: body schema check error: %s\n", res->GetError().c_str());
		err_json = ValidationErrorJsonBody("Body schema validation failed", "value_error");
		return false;
	}
	auto chunk = res->Fetch();
	bool ok = chunk && chunk->size() > 0 && !chunk->GetValue(0, 0).IsNull() && chunk->GetValue(0, 0).GetValue<bool>();
	if (ok) {
		return true;
	}
	// Recover a client-facing message from the bare throw.
	string msg = "Body schema validation failed";
	auto strict = con.Query("SELECT json_schema_validate(?::JSON, ?::JSON)", Value(schema), Value(raw_body));
	if (strict->HasError()) {
		msg = strict->GetError();
		const string prefixes[] = {"Invalid Input Error: ", "Invalid Error: ", "Binder Error: "};
		for (auto &p : prefixes) {
			if (StringUtil::StartsWith(msg, p)) {
				msg = msg.substr(p.size());
				break;
			}
		}
		if (msg.size() > 200) {
			msg = msg.substr(0, 200);
		}
	}
	err_json = ValidationErrorJson("body", "_schema", msg, "value_error");
	return false;
}

//! True if type is a signed/unsigned integral type (not float/decimal).
bool IsIntegralType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return true;
	default:
		return false;
	}
}

bool IsUnsignedIntegralType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return true;
	default:
		return false;
	}
}

//! FastAPI/Pydantic strict int: only optional leading '-' and digits — no
//! floats ("1.5"), scientific ("1e2"), hex ("0x10"), or surrounding spaces.
bool IsStrictIntegerString(const string &s, bool allow_negative) {
	if (s.empty()) {
		return false;
	}
	idx_t i = 0;
	if (s[0] == '-') {
		if (!allow_negative || s.size() == 1) {
			return false;
		}
		i = 1;
	}
	for (; i < s.size(); i++) {
		if (s[i] < '0' || s[i] > '9') {
			return false;
		}
	}
	return true;
}

bool IsNumericType(const LogicalType &type) {
	return IsIntegralType(type) || type.id() == LogicalTypeId::FLOAT || type.id() == LogicalTypeId::DOUBLE ||
	       type.id() == LogicalTypeId::DECIMAL;
}

//! Look up a PARAM spec by name (case-insensitive).
const QuackapiParamSpec *FindParamSpec(const vector<QuackapiParamSpec> &specs, const string &name) {
	for (auto &s : specs) {
		if (StringUtil::Lower(s.name) == StringUtil::Lower(name)) {
			return &s;
		}
	}
	return nullptr;
}

//! Apply FastAPI-style numeric/string constraints. Returns false and fills
//! msg/type when violated.
bool CheckParamConstraints(const QuackapiParamSpec &spec, const string &raw, const Value &bound, string &msg,
                           string &err_type) {
	// String length constraints use the raw request string (FastAPI min_length/max_length).
	if (spec.has_min_length && raw.size() < spec.min_length) {
		msg = StringUtil::Format("String should have at least %llu characters",
		                         (unsigned long long)spec.min_length);
		err_type = "string_too_short";
		return false;
	}
	if (spec.has_max_length && raw.size() > spec.max_length) {
		msg = StringUtil::Format("String should have at most %llu characters",
		                         (unsigned long long)spec.max_length);
		err_type = "string_too_long";
		return false;
	}
	if (!spec.has_ge && !spec.has_gt && !spec.has_le && !spec.has_lt) {
		return true;
	}
	if (bound.IsNull()) {
		return true;
	}
	// Numeric constraints need a number.
	double num = 0;
	if (IsNumericType(bound.type()) || bound.type().id() == LogicalTypeId::BOOLEAN) {
		Value dbl;
		string cerr;
		if (!bound.DefaultTryCastAs(LogicalType::DOUBLE, dbl, &cerr) || dbl.IsNull()) {
			return true; // non-numeric bound — skip numeric constraints
		}
		num = dbl.GetValue<double>();
	} else {
		// Try parse raw as double for VARCHAR-bound numbers
		try {
			num = std::stod(raw);
		} catch (...) {
			return true;
		}
	}
	if (spec.has_ge && !(num >= spec.ge)) {
		msg = StringUtil::Format("Input should be greater than or equal to %g", spec.ge);
		err_type = "greater_than_equal";
		return false;
	}
	if (spec.has_gt && !(num > spec.gt)) {
		msg = StringUtil::Format("Input should be greater than %g", spec.gt);
		err_type = "greater_than";
		return false;
	}
	if (spec.has_le && !(num <= spec.le)) {
		msg = StringUtil::Format("Input should be less than or equal to %g", spec.le);
		err_type = "less_than_equal";
		return false;
	}
	if (spec.has_lt && !(num < spec.lt)) {
		msg = StringUtil::Format("Input should be less than %g", spec.lt);
		err_type = "less_than";
		return false;
	}
	return true;
}

//! Bind a raw string (or default) into a BoundParameterData, applying strict
//! integer rules. On failure sets 422 body pieces via out params.
bool BindParamValue(const string &raw, const LogicalType &expected, const string &loc_kind, const string &param_name,
                    BoundParameterData &out, string &err_json) {
	// Strict integers: reject non-digit forms before DuckDB TryCast rounds them.
	if (IsIntegralType(expected)) {
		if (!IsStrictIntegerString(raw, !IsUnsignedIntegralType(expected))) {
			err_json = ValidationErrorJson(loc_kind, param_name, "Input should be a valid integer", "type_error");
			return false;
		}
	}
	Value raw_value(raw);
	if (expected.id() != LogicalTypeId::VARCHAR && expected.id() != LogicalTypeId::UNKNOWN) {
		Value casted;
		string cast_error;
		if (!raw_value.DefaultTryCastAs(expected, casted, &cast_error)) {
			err_json = ValidationErrorJson(loc_kind, param_name, "Input should be a valid " + expected.ToString(),
			                               "type_error");
			return false;
		}
		out = BoundParameterData(casted);
	} else {
		// No concrete type from the planner — still reject non-strict ints when
		// the raw string looks like a broken integer (contains '.' or 'e'/'E' or
		// spaces) only if it is otherwise "almost" an int? Leave as VARCHAR;
		// execute-time cast will convert. But for mixed $limit::INTEGER where
		// type is UNKNOWN, we still want strict rejection of "1.5"/"1e2".
		// Heuristic: if the string is non-empty and not a strict integer AND
		// contains only number-like chars (digits, ., e, E, +, -), try strict
		// int fail when it fails IsStrictIntegerString but would TryCast to int.
		// Safer approach used below in HandleRequest for UNKNOWN types with
		// PARAM type_name INTEGER, and a pre-check for number-like non-integers.
		out = BoundParameterData(raw_value);
	}
	return true;
}

struct RouteMatch {
	bool matched = false;
	QuackapiRoute route;
	// captured path params (name -> raw value)
	vector<std::pair<string, string>> path_params;
};

struct StreamMatch {
	bool matched = false;
	QuackapiStream stream;
	vector<std::pair<string, string>> path_params;
};

//! Format one SSE event from a result row. Includes `id:` when a column named
//! `id` (case-insensitive) is present and non-null.
string FormatSseEvent(const vector<string> &names, const vector<Value> &cols) {
	string event;
	idx_t id_col = names.size();
	for (idx_t c = 0; c < names.size(); c++) {
		if (StringUtil::Lower(names[c]) == "id") {
			id_col = c;
			break;
		}
	}
	if (id_col < cols.size() && !cols[id_col].IsNull()) {
		event += "id: ";
		event += cols[id_col].ToString();
		event += "\n";
	}
	event += "data: {";
	bool first = true;
	for (idx_t c = 0; c < names.size() && c < cols.size(); c++) {
		if (!first) {
			event += ",";
		}
		first = false;
		event += "\"" + JsonEscape(names[c]) + "\":" + ValueToJson(cols[c]);
	}
	event += "}\n\n";
	return event;
}

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

//! Trailing-slash presence for paths other than "/".
bool HasTrailingSlash(const string &path) {
	return path.size() > 1 && path.back() == '/';
}

string StripTrailingSlash(const string &path) {
	if (HasTrailingSlash(path)) {
		return path.substr(0, path.size() - 1);
	}
	return path;
}

//! Match a request path against a route pattern. ':name' and '{name}' segments
//! capture; all other segments must match exactly.
//! Trailing-slash is significant (Starlette parity): `/users` ≠ `/users/`.
bool MatchPattern(const string &pattern, const string &path, vector<std::pair<string, string>> &captures) {
	// Exact trailing-slash policy: patterns without trailing slash do not match
	// paths with one (and vice versa). SplitPath collapses empty segments, so
	// enforce slash equality explicitly.
	if (HasTrailingSlash(pattern) != HasTrailingSlash(path)) {
		return false;
	}
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

//! Rebuild query string from httplib params (order not guaranteed; fine for redirects).
string BuildQueryString(const duckdb_httplib::Request &req) {
	if (req.params.empty()) {
		return string();
	}
	string qs;
	bool first = true;
	for (auto &kv : req.params) {
		if (!first) {
			qs += "&";
		}
		first = false;
		qs += duckdb_httplib::encode_query_component(kv.first);
		qs += "=";
		qs += duckdb_httplib::encode_query_component(kv.second);
	}
	return qs;
}

//! Wire name for a HEADER/COOKIE PARAM (FastAPI Header convert_underscores).
string ParamWireName(const QuackapiParamSpec &spec) {
	if (!spec.external_name.empty()) {
		return spec.external_name;
	}
	if (spec.source == QuackapiParamSource::HEADER) {
		// user_agent → user-agent (matched case-insensitively against User-Agent)
		string out = spec.name;
		for (char &c : out) {
			if (c == '_') {
				c = '-';
			}
		}
		return out;
	}
	// COOKIE / QUERY: param name as-is
	return spec.name;
}

//! Parse Cookie header into name → value (first wins; values unquoted).
case_insensitive_map_t<string> ParseCookieHeader(const string &cookie_header) {
	case_insensitive_map_t<string> out;
	idx_t i = 0;
	while (i < cookie_header.size()) {
		// skip whitespace and separators
		while (i < cookie_header.size() &&
		       (cookie_header[i] == ' ' || cookie_header[i] == ';' || cookie_header[i] == '\t')) {
			i++;
		}
		if (i >= cookie_header.size()) {
			break;
		}
		idx_t start = i;
		while (i < cookie_header.size() && cookie_header[i] != '=' && cookie_header[i] != ';') {
			i++;
		}
		string name = cookie_header.substr(start, i - start);
		StringUtil::Trim(name);
		string val;
		if (i < cookie_header.size() && cookie_header[i] == '=') {
			i++;
			idx_t vstart = i;
			while (i < cookie_header.size() && cookie_header[i] != ';') {
				i++;
			}
			val = cookie_header.substr(vstart, i - vstart);
			StringUtil::Trim(val);
			// Strip surrounding quotes if present
			if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
				val = val.substr(1, val.size() - 2);
			}
		}
		if (!name.empty() && out.find(name) == out.end()) {
			out[name] = val;
		}
		if (i < cookie_header.size() && cookie_header[i] == ';') {
			i++;
		}
	}
	return out;
}

//! Case-insensitive header lookup by wire name.
bool FindHeaderValue(const case_insensitive_map_t<string> &headers, const string &wire_name, string &out) {
	auto it = headers.find(wire_name);
	if (it != headers.end()) {
		out = it->second;
		return true;
	}
	return false;
}

//! True if column name is a special response control column (stripped from body).
bool IsSpecialResponseColumn(const string &name) {
	auto lower = StringUtil::Lower(name);
	return lower == "location" || lower == "set_cookie" || lower == "set-cookie";
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

bool MethodListContains(const vector<string> &list, const string &method) {
	for (auto &s : list) {
		if (s == method) {
			return true;
		}
	}
	return false;
}

} // namespace

QuackapiHttpServer::QuackapiHttpServer(DatabaseInstance &db, const string &host_p, int port_p,
                                       const QuackapiServeOptions &opts)
    : db_ptr(db.shared_from_this()), host(host_p), port(port_p), cors_origins(opts.cors_origins), options(opts),
      started_at(std::chrono::steady_clock::now()) {
	server = make_uniq<duckdb_httplib::Server>();

	// Static files (FastAPI StaticFiles equivalent). httplib checks file
	// requests before route handlers, so API routes always win over files.
	if (!opts.static_dir.empty() && !server->set_mount_point("/", opts.static_dir)) {
		throw IOException("quackapi: static_dir \"%s\" is not a directory", opts.static_dir);
	}

	// Transport defaults (overridable via serve opts) — correct-by-default for servers.
	int32_t workers = opts.worker_threads > 0 ? opts.worker_threads
	                                          : static_cast<int32_t>(QUACKAPI_DEFAULT_WORKER_THREADS);
	server->new_task_queue = [workers] {
		return new duckdb_httplib::ThreadPool(static_cast<size_t>(workers));
	};
	server->set_keep_alive_max_count(opts.keep_alive_max_count > 0
	                                     ? static_cast<size_t>(opts.keep_alive_max_count)
	                                     : QUACKAPI_DEFAULT_KEEP_ALIVE_MAX);
	server->set_keep_alive_timeout(opts.keep_alive_timeout_sec > 0
	                                   ? static_cast<time_t>(opts.keep_alive_timeout_sec)
	                                   : QUACKAPI_DEFAULT_KEEP_ALIVE_TIMEOUT_SEC);
	server->set_read_timeout(opts.read_timeout_sec > 0 ? static_cast<time_t>(opts.read_timeout_sec)
	                                                   : QUACKAPI_DEFAULT_IO_TIMEOUT_SEC);
	server->set_write_timeout(opts.write_timeout_sec > 0 ? static_cast<time_t>(opts.write_timeout_sec)
	                                                     : QUACKAPI_DEFAULT_IO_TIMEOUT_SEC);
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
	// HEAD is dispatched to get_handlers_ by httplib (no separate Head API).
	// Automatic HEAD-for-GET is handled inside HandleRequest.
	// OPTIONS: CORS preflight + Allow listing for registered paths.
	server->Options(".*", handler);

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

string QuackapiHttpServer::NextRequestId(DatabaseInstance &db) {
	// Prefer community tsid() when probe succeeded; else core uuidv7 (C++ — no SQL).
	if (options.request_id_source == "tsid") {
		try {
			Connection con(db);
			auto res = con.Query("SELECT tsid()");
			if (!res->HasError()) {
				auto chunk = res->Fetch();
				if (chunk && chunk->size() > 0 && !chunk->GetValue(0, 0).IsNull()) {
					return chunk->GetValue(0, 0).ToString();
				}
			}
		} catch (...) {
			// fall through to uuidv7
		}
	}
	return UUID::ToString(UUIDv7::GenerateRandomUUID());
}

void QuackapiHttpServer::EmitAccessLog(const duckdb_httplib::Request &req, const duckdb_httplib::Response &res,
                                       const string &request_id, double latency_ms) {
	if (!options.access_log || options.log_level < QuackapiLogLevel::INFO) {
		return;
	}
	// Structured JSON (one line) — method, path, status, latency_ms, request_id, bytes.
	size_t bytes = res.body.size();
	// Prefer Content-Length when set; body may be empty for HEAD.
	auto cl = res.headers.find("Content-Length");
	if (cl != res.headers.end()) {
		try {
			bytes = static_cast<size_t>(std::stoull(cl->second));
		} catch (...) {
		}
	}
	// Escape path for JSON (minimal: quotes + backslash + control chars).
	string path_esc;
	path_esc.reserve(req.path.size() + 8);
	for (unsigned char c : req.path) {
		if (c == '"' || c == '\\') {
			path_esc += '\\';
			path_esc += static_cast<char>(c);
		} else if (c < 0x20) {
			char buf[8];
			snprintf(buf, sizeof(buf), "\\u%04x", c);
			path_esc += buf;
		} else {
			path_esc += static_cast<char>(c);
		}
	}
	fprintf(stderr,
	        "{\"type\":\"access\",\"method\":\"%s\",\"path\":\"%s\",\"status\":%d,"
	        "\"latency_ms\":%.3f,\"request_id\":\"%s\",\"bytes\":%llu}\n",
	        req.method.c_str(), path_esc.c_str(), res.status, latency_ms, request_id.c_str(),
	        (unsigned long long)bytes);
	fflush(stderr);
}

void QuackapiHttpServer::ApplyCorsHeaders(const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
	if (cors_origins.empty()) {
		return;
	}
	string origin;
	auto origin_it = req.headers.find("Origin");
	if (origin_it != req.headers.end()) {
		origin = origin_it->second;
	}

	string allow_origin;
	if (cors_origins == "*") {
		// When credentials are not used, * is fine; if a specific Origin was
		// sent, echo it so browser clients with credentials still work.
		allow_origin = origin.empty() ? "*" : origin;
	} else {
		// Comma-separated allow-list (trim whitespace per entry).
		auto parts = StringUtil::Split(cors_origins, ',');
		for (auto &part : parts) {
			string trimmed = part;
			StringUtil::Trim(trimmed);
			if (!trimmed.empty() && trimmed == origin) {
				allow_origin = origin;
				break;
			}
		}
		if (allow_origin.empty()) {
			// Origin not allowed — do not set CORS headers.
			return;
		}
	}

	res.set_header("Access-Control-Allow-Origin", allow_origin);
	res.set_header("Access-Control-Allow-Methods", "GET, HEAD, POST, PUT, DELETE, PATCH, OPTIONS");
	// Echo requested headers when present; otherwise a sensible default set.
	string req_headers;
	auto acrh = req.headers.find("Access-Control-Request-Headers");
	if (acrh != req.headers.end() && !acrh->second.empty()) {
		req_headers = acrh->second;
	} else {
		req_headers = "Authorization, Content-Type, X-API-Key";
	}
	res.set_header("Access-Control-Allow-Headers", req_headers);
	res.set_header("Access-Control-Max-Age", "600");
	// When we echo a specific origin, advertise that the response may vary.
	if (allow_origin != "*") {
		res.set_header("Vary", "Origin");
	}
}

void QuackapiHttpServer::HandleRequest(const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
	// Always attach CORS headers when configured (including error responses).
	// Stamp X-Request-ID + emit structured access log on every exit path.
	const auto t0 = std::chrono::steady_clock::now();
	string request_id; // filled once db is available; may be empty on 503 shutdown

	auto finish = [&]() {
		if (!request_id.empty()) {
			res.set_header("X-Request-ID", request_id);
		}
		ApplyCorsHeaders(req, res);
		auto t1 = std::chrono::steady_clock::now();
		double latency_ms =
		    std::chrono::duration<double, std::milli>(t1 - t0).count();
		EmitAccessLog(req, res, request_id.empty() ? string("-") : request_id, latency_ms);
	};

	auto db = db_ptr.lock();
	if (!db) {
		SetJson(res, 503, "{\"detail\":\"database shutting down\"}");
		finish();
		return;
	}
	request_id = NextRequestId(*db);

	// Built-in health routes (also registered in quackapi_routes() for listing).
	// Liveness: process accepting HTTP. Readiness: DB handle + version + uptime.
	if (options.health_routes && (req.method == "GET" || req.method == "HEAD")) {
		if (req.path == "/health" || req.path == "/health/") {
			// Object body (not row-array) — standard k8s/load-balancer shape.
			SetJson(res, 200, "{\"status\":\"ok\"}");
			finish();
			return;
		}
		if (req.path == "/healthz" || req.path == "/healthz/") {
			// Readiness: verify the DB handle can run a trivial query.
			string version = "unknown";
			bool ready = false;
			try {
				Connection con(*db);
				auto resq = con.Query("SELECT version()");
				if (!resq->HasError()) {
					auto chunk = resq->Fetch();
					if (chunk && chunk->size() > 0 && !chunk->GetValue(0, 0).IsNull()) {
						version = chunk->GetValue(0, 0).ToString();
						ready = true;
					}
				}
			} catch (...) {
				ready = false;
			}
			auto uptime_sec =
			    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at)
			        .count();
			if (ready) {
				SetJson(res, 200,
				        StringUtil::Format(
				            "{\"status\":\"ok\",\"version\":\"%s\",\"uptime_sec\":%lld,\"request_id_source\":\"%s\"}",
				            JsonEscape(version), (long long)uptime_sec,
				            JsonEscape(options.request_id_source.empty() ? "uuidv7" : options.request_id_source)));
			} else {
				SetJson(res, 503, "{\"status\":\"not_ready\",\"detail\":\"database handle check failed\"}");
			}
			finish();
			return;
		}
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
			// httplib skips writing the body for HEAD while preserving Content-Length.
			finish();
			return;
		}
		if (req.path == "/docs" || req.path == "/docs/") {
			res.status = 200;
			res.set_content(OpenApiDocsHtml(), "text/html; charset=utf-8");
			finish();
			return;
		}
		if (req.path == "/redoc" || req.path == "/redoc/") {
			res.status = 200;
			res.set_content(OpenApiRedocHtml(), "text/html; charset=utf-8");
			finish();
			return;
		}
	}

	// Built-in OPTIONS for docs paths: 204 preflight only when CORS is on;
	// otherwise 405 like FastAPI without CORSMiddleware.
	if (req.method == "OPTIONS" &&
	    (req.path == "/openapi.json" || req.path == "/docs" || req.path == "/docs/" || req.path == "/redoc" ||
	     req.path == "/redoc/")) {
		if (cors_origins.empty()) {
			res.set_header("Allow", "GET, HEAD");
			SetJson(res, 405, "{\"detail\":\"Method Not Allowed\"}");
		} else {
			res.status = 204;
			res.set_header("Allow", "GET, HEAD, OPTIONS");
			res.body.clear();
		}
		finish();
		return;
	}

	// Find a route: method + pattern. Collect methods for Allow on 405.
	// HEAD automatically matches GET when no explicit HEAD route exists.
	// Streams (CREATE STREAM) are matched the same way; ordinary routes win
	// when both register the same path+method.
	RouteMatch match;
	StreamMatch stream_match;
	bool path_matched_other_method = false;
	vector<string> methods_for_path;
	auto &qa_state = QuackapiState::Get(*db);
	auto routes = qa_state.SnapshotRoutes();
	auto streams = qa_state.SnapshotStreams();
	for (auto &route : routes) {
		vector<std::pair<string, string>> captures;
		if (MatchPattern(route.pattern, req.path, captures)) {
			if (!MethodListContains(methods_for_path, route.method)) {
				// Collect unique methods (order: first seen).
				methods_for_path.push_back(route.method);
			}
			if (route.method == req.method) {
				match.matched = true;
				match.route = route;
				match.path_params = std::move(captures);
				// Prefer exact method match; keep scanning only for Allow list.
			} else if (!match.matched) {
				path_matched_other_method = true;
			}
		}
	}
	for (auto &stream : streams) {
		vector<std::pair<string, string>> captures;
		if (MatchPattern(stream.pattern, req.path, captures)) {
			if (!MethodListContains(methods_for_path, stream.method)) {
				methods_for_path.push_back(stream.method);
			}
			if (stream.method == req.method) {
				// Only take the stream if no ordinary route already matched.
				if (!match.matched && !stream_match.matched) {
					stream_match.matched = true;
					stream_match.stream = stream;
					stream_match.path_params = std::move(captures);
				}
			} else if (!match.matched && !stream_match.matched) {
				path_matched_other_method = true;
			}
		}
	}
	// Auto-HEAD: if HEAD and no explicit HEAD route, reuse the GET handler
	// (routes first, then streams).
	if (!match.matched && !stream_match.matched && req.method == "HEAD") {
		for (auto &route : routes) {
			vector<std::pair<string, string>> captures;
			if (MatchPattern(route.pattern, req.path, captures) && route.method == "GET") {
				match.matched = true;
				match.route = route;
				match.path_params = std::move(captures);
				if (!MethodListContains(methods_for_path, "HEAD")) {
					methods_for_path.push_back("HEAD");
				}
				break;
			}
		}
		if (!match.matched) {
			for (auto &stream : streams) {
				vector<std::pair<string, string>> captures;
				if (MatchPattern(stream.pattern, req.path, captures) && stream.method == "GET") {
					stream_match.matched = true;
					stream_match.stream = stream;
					stream_match.path_params = std::move(captures);
					if (!MethodListContains(methods_for_path, "HEAD")) {
						methods_for_path.push_back("HEAD");
					}
					break;
				}
			}
		}
	}

	// OPTIONS: FastAPI without CORSMiddleware returns 405 for unregistered
	// OPTIONS on an otherwise-valid path. With CORS configured we answer
	// preflight with 204 + Allow (+ Access-Control-* via finish()).
	if (req.method == "OPTIONS") {
		if (!methods_for_path.empty() || path_matched_other_method) {
			if (MethodListContains(methods_for_path, "GET") && !MethodListContains(methods_for_path, "HEAD")) {
				methods_for_path.push_back("HEAD");
			}
			if (cors_origins.empty()) {
				// Match Starlette/FastAPI default: OPTIONS is not allowed.
				res.set_header("Allow", StringUtil::Join(methods_for_path, ", "));
				SetJson(res, 405, "{\"detail\":\"Method Not Allowed\"}");
				finish();
				return;
			}
			if (!MethodListContains(methods_for_path, "OPTIONS")) {
				methods_for_path.push_back("OPTIONS");
			}
			string allow = StringUtil::Join(methods_for_path, ", ");
			res.status = 204;
			res.set_header("Allow", allow);
			res.body.clear();
			finish();
			return;
		}
		// Unknown path: 404 (no route to preflight).
		SetJson(res, 404, "{\"detail\":\"Not Found\"}");
		finish();
		return;
	}

	if (!match.matched && !stream_match.matched) {
		// Starlette redirect_slashes: if the alternate trailing-slash form would
		// match a registered route or stream, 307 to that path (preserve query).
		// Built-in docs paths already accept both forms above.
		if (!path_matched_other_method && methods_for_path.empty() && req.path != "/") {
			string alt_path;
			if (HasTrailingSlash(req.path)) {
				alt_path = StripTrailingSlash(req.path);
			} else {
				alt_path = req.path + "/";
			}
			bool alt_matches = false;
			for (auto &route : routes) {
				vector<std::pair<string, string>> captures;
				if (!MatchPattern(route.pattern, alt_path, captures)) {
					continue;
				}
				// Any method registration is enough to redirect (Starlette).
				alt_matches = true;
				break;
			}
			if (!alt_matches) {
				for (auto &stream : streams) {
					vector<std::pair<string, string>> captures;
					if (MatchPattern(stream.pattern, alt_path, captures)) {
						alt_matches = true;
						break;
					}
				}
			}
			if (alt_matches) {
				string location = alt_path;
				string qs = BuildQueryString(req);
				if (!qs.empty()) {
					location += "?" + qs;
				}
				res.status = 307;
				res.set_header("Location", location);
				res.body.clear();
				finish();
				return;
			}
		}
		if (path_matched_other_method || !methods_for_path.empty()) {
			// Ensure HEAD is advertised when GET is registered.
			if (MethodListContains(methods_for_path, "GET") && !MethodListContains(methods_for_path, "HEAD")) {
				methods_for_path.push_back("HEAD");
			}
			// Only advertise OPTIONS when CORS is on (preflight is accepted).
			if (!cors_origins.empty() && !MethodListContains(methods_for_path, "OPTIONS")) {
				methods_for_path.push_back("OPTIONS");
			}
			res.set_header("Allow", StringUtil::Join(methods_for_path, ", "));
			SetJson(res, 405, "{\"detail\":\"Method Not Allowed\"}");
		} else {
			SetJson(res, 404, "{\"detail\":\"Not Found\"}");
		}
		finish();
		return;
	}

	// ---- CREATE STREAM (SSE) path — no auth schemes on streams in v1 ----
	if (!match.matched && stream_match.matched) {
		try {
			auto headers = CollectHeaders(req);
			// HEAD: headers only — do not install a long-lived provider.
			if (req.method == "HEAD") {
				res.status = 200;
				res.set_header("Content-Type", "text/event-stream");
				res.set_header("Cache-Control", "no-cache");
				res.set_header("X-Accel-Buffering", "no");
				res.body.clear();
				finish();
				return;
			}

			// Bind path + query (+ Last-Event-ID → $last_id) before installing provider.
			auto con = make_shared_ptr<Connection>(*db);
			auto prepared = con->Prepare(stream_match.stream.handler_sql);
			if (prepared->HasError()) {
				SetInternalError(res, prepared->GetError());
				finish();
				return;
			}

			case_insensitive_map_t<string> provided;
			for (auto &kv : req.params) {
				provided[kv.first] = kv.second;
			}
			for (auto &kv : stream_match.path_params) {
				provided[kv.first] = kv.second;
			}
			// Last-Event-ID header → last_id (query ?last_id= wins if both set).
			if (provided.find("last_id") == provided.end()) {
				string last_event_id;
				if (FindHeaderValue(headers, "Last-Event-ID", last_event_id) && !last_event_id.empty()) {
					provided["last_id"] = last_event_id;
				}
			}

			auto expected_types = prepared->GetExpectedParameterTypes();
			case_insensitive_map_t<BoundParameterData> named_values;
			for (auto &entry : prepared->named_param_map) {
				auto &param_name = entry.first;
				auto type_it = expected_types.find(param_name);
				LogicalType expected = LogicalType::UNKNOWN;
				if (type_it != expected_types.end()) {
					expected = type_it->second;
				}
				auto it = provided.find(param_name);
				if (it == provided.end()) {
					// Optional last_id: missing → SQL NULL so WHERE id > $last_id works with COALESCE.
					if (param_name == "last_id") {
						named_values[param_name] = BoundParameterData(Value());
						continue;
					}
					string loc = IsPathParam(stream_match.stream.pattern, param_name) ? "path" : "query";
					SetJson(res, 422, ValidationErrorJson(loc, param_name, "Field required", "missing"));
					finish();
					return;
				}
				string loc = IsPathParam(stream_match.stream.pattern, param_name) ? "path" : "query";
				BoundParameterData bound;
				string err_json;
				if (!BindParamValue(it->second, expected, loc, param_name, bound, err_json)) {
					SetJson(res, 422, err_json);
					finish();
					return;
				}
				named_values[param_name] = bound;
			}

			// Prove the first execute works before committing to chunked transfer.
			{
				auto probe = prepared->Execute(named_values, true);
				if (probe->HasError()) {
					SetInternalError(res, probe->GetError());
					finish();
					return;
				}
				// Drop probe; provider re-executes so the client sees a clean stream.
			}

			struct SseProviderState {
				shared_ptr<Connection> con;
				string handler_sql;
				case_insensitive_map_t<BoundParameterData> named_values;
				int64_t interval_ms = 0;
				unique_ptr<PreparedStatement> prepared;
				unique_ptr<QueryResult> result;
				bool need_execute = true;
				bool closed = false;
			};
			auto state = make_shared_ptr<SseProviderState>();
			state->con = con;
			state->handler_sql = stream_match.stream.handler_sql;
			state->named_values = std::move(named_values);
			state->interval_ms = stream_match.stream.interval_ms;
			// Re-prepare owned by provider state (original prepared is local).
			state->prepared = state->con->Prepare(state->handler_sql);
			if (state->prepared->HasError()) {
				SetInternalError(res, state->prepared->GetError());
				finish();
				return;
			}

			res.status = 200;
			res.set_header("Cache-Control", "no-cache");
			res.set_header("X-Accel-Buffering", "no");
			// CORS before provider install — headers already on res when provider runs.
			ApplyCorsHeaders(req, res);

			res.set_chunked_content_provider(
			    "text/event-stream",
			    [state](size_t /*offset*/, duckdb_httplib::DataSink &sink) -> bool {
				    if (state->closed) {
					    sink.done();
					    return true;
				    }
				    try {
					    if (state->need_execute) {
						    state->result = state->prepared->Execute(state->named_values, true);
						    if (state->result->HasError()) {
							    fprintf(stderr, "quackapi stream execute error: %s\n",
							            state->result->GetError().c_str());
							    state->closed = true;
							    return false;
						    }
						    state->need_execute = false;
					    }

					    auto chunk = state->result->Fetch();
					    if (!chunk || chunk->size() == 0) {
						    state->result.reset();
						    if (state->interval_ms <= 0) {
							    state->closed = true;
							    sink.done();
							    return true;
						    }
						    // Polling interval: sleep then re-run SELECT (compose cron-style).
						    // Split sleep so disconnect can surface via write fail next loop.
						    auto remaining = state->interval_ms;
						    while (remaining > 0) {
							    auto step = remaining > 100 ? 100 : remaining;
							    std::this_thread::sleep_for(std::chrono::milliseconds(step));
							    remaining -= step;
							    if (!sink.is_writable()) {
								    state->closed = true;
								    return false;
							    }
						    }
						    state->need_execute = true;
						    return true;
					    }

					    auto &names = state->result->names;
					    string buf;
					    for (idx_t row = 0; row < chunk->size(); row++) {
						    vector<Value> cols(chunk->ColumnCount());
						    for (idx_t col = 0; col < chunk->ColumnCount(); col++) {
							    cols[col] = chunk->GetValue(col, row);
						    }
						    buf += FormatSseEvent(names, cols);
					    }
					    if (!buf.empty() && !sink.write(buf.data(), buf.size())) {
						    state->closed = true;
						    return false;
					    }
					    return true;
				    } catch (std::exception &ex) {
					    fprintf(stderr, "quackapi stream provider exception: %s\n", ex.what());
					    state->closed = true;
					    return false;
				    } catch (...) {
					    fprintf(stderr, "quackapi stream provider: unknown exception\n");
					    state->closed = true;
					    return false;
				    }
			    },
			    [state](bool /*success*/) {
				    try {
					    if (state->con) {
						    state->con->Interrupt();
					    }
				    } catch (...) {
				    }
				    state->result.reset();
				    state->prepared.reset();
				    state->closed = true;
			    });
			// Do not call finish() again — CORS already applied; provider owns body.
			return;
		} catch (std::exception &ex) {
			SetInternalError(res, ex.what());
			finish();
			return;
		} catch (...) {
			SetInternalError(res, "unknown exception");
			finish();
			return;
		}
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
		finish();
		return;
	}
	// auth_result.claims is ready for $claims_<k> binding below.

	// ---- ROW ACCESS + MASKING (claims-keyed policies on tables) ----
	// When the handler references a table with RAP/masking bindings, wrap
	// table refs as secure subqueries. Unauthenticated requests fail closed.
	bool authenticated = !match.route.require_auth.empty() && auth_result.ok;
	bool deny_unauth = false;
	string handler_sql =
	    RewriteHandlerWithPolicies(*db, match.route.handler_sql, authenticated, deny_unauth);
	if (deny_unauth) {
		SetJson(res, 403, "{\"detail\":\"Policy denies unauthenticated access\"}");
		finish();
		return;
	}

	try {
		Connection con(*db);
		auto prepared = con.Prepare(handler_sql);
		if (prepared->HasError()) {
			SetInternalError(res, prepared->GetError());
			finish();
			return;
		}

		// Request params: path captures shadow query params of the same name.
		// Body fields (JSON / form / multipart) fill remaining names with loc=body.
		case_insensitive_map_t<std::pair<string, string>> provided; // name -> (loc, raw)
		string media = ContentTypeMedia(headers);
		bool form_ct = IsFormUrlEncodedMediaType(media);
		bool multipart_ct = IsMultipartMediaType(media) || req.is_multipart_form_data();

		// Query string params. When CT is form-urlencoded, httplib merges the body
		// into req.params — re-parse body as body loc and only treat non-body keys
		// from params as query (best-effort: form fields overwrite with body loc).
		for (auto &kv : req.params) {
			provided[kv.first] = {"query", kv.second};
		}
		for (auto &kv : match.path_params) {
			provided[kv.first] = {"path", kv.second};
		}

		// ---- HEADER / COOKIE PARAMS (FastAPI Header / Cookie) ----
		// Declared via PARAM <name> HEADER [wire] | COOKIE [wire]. Wire defaults:
		// HEADER: underscore→hyphen (x_token → x-token); COOKIE: param name.
		case_insensitive_map_t<string> cookies;
		bool cookies_parsed = false;
		for (auto &pspec : match.route.params) {
			if (pspec.source == QuackapiParamSource::HEADER) {
				string wire = ParamWireName(pspec);
				string val;
				if (FindHeaderValue(headers, wire, val)) {
					// Header fills only if not already path-bound (path still wins).
					if (provided.find(pspec.name) == provided.end() ||
					    provided[pspec.name].first != "path") {
						provided[pspec.name] = {"header", val};
					}
				}
			} else if (pspec.source == QuackapiParamSource::COOKIE) {
				if (!cookies_parsed) {
					string cookie_hdr;
					if (FindHeaderValue(headers, "Cookie", cookie_hdr)) {
						cookies = ParseCookieHeader(cookie_hdr);
					}
					cookies_parsed = true;
				}
				string wire = ParamWireName(pspec);
				auto cit = cookies.find(wire);
				if (cit != cookies.end()) {
					if (provided.find(pspec.name) == provided.end() ||
					    provided[pspec.name].first != "path") {
						provided[pspec.name] = {"cookie", cit->second};
					}
				}
			}
		}

		// ---- REQUEST BODY BINDING (POST/PUT/PATCH) ----
		// FastAPI parity: JSON body fields, form-urlencoded, multipart files,
		// malformed JSON → 422 json_invalid, wrong CT → 422 model_attributes_type.
		bool body_method = IsBodyMethod(req.method);
		bool has_body_named = prepared->named_param_map.find("body") != prepared->named_param_map.end();
		// Does the handler still need any non-path/non-claims param not yet provided?
		bool needs_body_fields = has_body_named || !match.route.body_schema.empty();
		if (!needs_body_fields) {
			for (auto &entry : prepared->named_param_map) {
				string ck;
				if (IsClaimsParam(entry.first, ck)) {
					continue;
				}
				if (entry.first == "body") {
					continue;
				}
				if (provided.find(entry.first) != provided.end()) {
					continue;
				}
				if (IsPathParam(match.route.pattern, entry.first)) {
					continue;
				}
				const QuackapiParamSpec *ps = FindParamSpec(match.route.params, entry.first);
				if (ps && ps->has_default) {
					continue;
				}
				// Header/Cookie params are not body-derived.
				if (ps && (ps->source == QuackapiParamSource::HEADER ||
				           ps->source == QuackapiParamSource::COOKIE)) {
					continue;
				}
				// File filename helpers are body-derived.
				needs_body_fields = true;
				break;
			}
		}

		if (body_method) {
			if (IsJsonMediaType(media) || (media.empty() && !req.body.empty() &&
			                               (req.body[0] == '{' || req.body[0] == '['))) {
				// JSON body path.
				if (req.body.empty() && !needs_body_fields && match.route.body_schema.empty()) {
					// Empty JSON body with fully-bound query/path params — ignore.
				} else {
					case_insensitive_map_t<string> body_fields;
					string err_json;
					if (!ExtractJsonBodyFields(con, req.body, body_fields, err_json)) {
						// Empty body with application/json and missing fields: if body
						// is empty, Extract returns json_invalid — correct for body models.
						// If CT is application/json and body empty but only query params
						// needed, we already skipped above.
						SetJson(res, 422, err_json);
						finish();
						return;
					}
					if (!match.route.body_schema.empty()) {
						if (!ValidateBodySchema(con, match.route.body_schema, req.body, err_json)) {
							SetJson(res, 422, err_json);
							finish();
							return;
						}
					}
					// Body fields fill missing names only (path/query win).
					for (auto &kv : body_fields) {
						if (provided.find(kv.first) == provided.end()) {
							provided[kv.first] = {"body", kv.second};
						}
					}
					// $body binds the raw JSON payload.
					if (has_body_named && provided.find("body") == provided.end()) {
						provided["body"] = {"body", req.body};
					}
				}
			} else if (form_ct) {
				case_insensitive_map_t<string> form_fields;
				ParseFormUrlEncoded(req.body, form_fields);
				for (auto &kv : form_fields) {
					// Form fields are body-located (FastAPI Form(...)).
					provided[kv.first] = {"body", kv.second};
				}
				if (has_body_named && provided.find("body") == provided.end()) {
					provided["body"] = {"body", req.body};
				}
			} else if (multipart_ct) {
				// Text fields
				for (auto &kv : req.form.fields) {
					provided[kv.first] = {"body", kv.second.content};
				}
				// Files: $name = content bytes, $name_filename = original filename
				for (auto &kv : req.form.files) {
					provided[kv.first] = {"body", kv.second.content};
					string fname_key = kv.first + "_filename";
					provided[fname_key] = {"body", kv.second.filename};
					// Convenience: single-file routes may bind $filename
					if (provided.find("filename") == provided.end()) {
						provided["filename"] = {"body", kv.second.filename};
					}
				}
				if (has_body_named && provided.find("body") == provided.end()) {
					provided["body"] = {"body", req.body};
				}
			} else if (!media.empty() && !req.body.empty() && needs_body_fields) {
				// Wrong Content-Type for a body-expecting route (FastAPI model_attributes_type).
				SetJson(res, 422,
				        ValidationErrorJsonBody(
				            "Input should be a valid dictionary or object to extract fields from",
				            "model_attributes_type"));
				finish();
				return;
			} else if (!match.route.body_schema.empty()) {
				// BODY SCHEMA requires JSON.
				SetJson(res, 422,
				        ValidationErrorJsonBody(
				            "Input should be a valid dictionary or object to extract fields from",
				            "model_attributes_type"));
				finish();
				return;
			}
		}

		// The database types the columns: cast each provided param to the type
		// the prepared statement expects. Cast failure -> 422, FastAPI-shaped.
		// $claims_* params are server-verified: bind from claims or SQL NULL
		// (never 422 for a missing claim).
		// PARAM … DEFAULT makes a query/path param optional (bind default/NULL).
		auto expected_types = prepared->GetExpectedParameterTypes();
		case_insensitive_map_t<BoundParameterData> named_values;
		// Track raw strings for execute-time conversion error → param name recovery.
		case_insensitive_map_t<std::pair<string, string>> bound_raw; // name -> (loc, raw)

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

			const QuackapiParamSpec *spec = FindParamSpec(match.route.params, param_name);
			auto type_it = expected_types.find(param_name);
			LogicalType expected = LogicalType::UNKNOWN;
			if (type_it != expected_types.end()) {
				expected = type_it->second;
			}
			// PARAM type_name can refine UNKNOWN/VARCHAR when the planner did not
			// surface a concrete type (e.g. LIMIT $limit::INTEGER sometimes).
			if ((expected.id() == LogicalTypeId::UNKNOWN || expected.id() == LogicalTypeId::VARCHAR) && spec &&
			    !spec->type_name.empty()) {
				auto tn = StringUtil::Upper(spec->type_name);
				if (tn == "INTEGER" || tn == "INT") {
					expected = LogicalType::INTEGER;
				} else if (tn == "BIGINT") {
					expected = LogicalType::BIGINT;
				} else if (tn == "SMALLINT") {
					expected = LogicalType::SMALLINT;
				} else if (tn == "TINYINT") {
					expected = LogicalType::TINYINT;
				} else if (tn == "HUGEINT") {
					expected = LogicalType::HUGEINT;
				} else if (tn == "DOUBLE") {
					expected = LogicalType::DOUBLE;
				} else if (tn == "FLOAT" || tn == "REAL") {
					expected = LogicalType::FLOAT;
				} else if (tn == "BOOLEAN" || tn == "BOOL") {
					expected = LogicalType::BOOLEAN;
				} else if (tn == "VARCHAR" || tn == "TEXT" || tn == "STRING") {
					expected = LogicalType::VARCHAR;
				}
			}

			auto it = provided.find(param_name);
			// Default loc for missing errors: path / header / cookie / query.
			string loc_kind = "query";
			if (IsPathParam(match.route.pattern, param_name)) {
				loc_kind = "path";
			} else if (spec && spec->source == QuackapiParamSource::HEADER) {
				loc_kind = "header";
			} else if (spec && spec->source == QuackapiParamSource::COOKIE) {
				loc_kind = "cookie";
			}
			string raw;
			bool from_default = false;

			if (it == provided.end()) {
				if (spec && spec->has_default) {
					from_default = true;
					if (spec->default_is_null) {
						named_values[param_name] = BoundParameterData(Value());
						// Constraints on absent optional NULL: skip (FastAPI).
						continue;
					}
					raw = spec->default_raw;
					// Defaults keep the declared source loc for error shape.
				} else {
					SetJson(res, 422, ValidationErrorJson(loc_kind, param_name, "Field required", "missing"));
					finish();
					return;
				}
			} else {
				raw = it->second.second;
				loc_kind = it->second.first;
			}

			// Strict integral check even when type is UNKNOWN: if the raw value
			// is number-like but not a strict integer, and a cast-to-int would
			// succeed via DuckDB rounding (1.5→2, 1e2→100), reject now.
			// When expected is integral we always require ^-?[0-9]+$.
			// When expected is UNKNOWN, apply the same if raw fails strict int
			// but DefaultTryCastAs to INTEGER succeeds (the FastAPI gap).
			if (IsIntegralType(expected)) {
				if (!IsStrictIntegerString(raw, !IsUnsignedIntegralType(expected))) {
					SetJson(res, 422,
					        ValidationErrorJson(loc_kind, param_name, "Input should be a valid integer",
					                            "type_error"));
					finish();
					return;
				}
			} else if (expected.id() == LogicalTypeId::UNKNOWN || expected.id() == LogicalTypeId::VARCHAR) {
				// If raw is not a strict integer but DuckDB would accept it as
				// INTEGER (float/scientific/hex-ish), reject so execute-time
				// `$param::INTEGER` cannot round. Plain non-numeric strings
				// ("abc") still reach execute and become 422 with the real name.
				if (!raw.empty() && !IsStrictIntegerString(raw, true)) {
					Value probe(raw);
					Value as_int;
					string cerr;
					if (probe.DefaultTryCastAs(LogicalType::INTEGER, as_int, &cerr)) {
						// Would cast — only reject number-like non-integers
						// (contain digit and non-digit). Pure text like "abc"
						// fails TryCast and is left for later.
						bool has_digit = false;
						bool has_non_digit = false;
						for (unsigned char c : raw) {
							if (c >= '0' && c <= '9') {
								has_digit = true;
							} else if (c != '-' && c != '+') {
								has_non_digit = true;
							}
						}
						if (has_digit && has_non_digit) {
							SetJson(res, 422,
							        ValidationErrorJson(loc_kind, param_name, "Input should be a valid integer",
							                            "type_error"));
							finish();
							return;
						}
					}
				}
			}

			BoundParameterData bound;
			string err_json;
			if (!BindParamValue(raw, expected, loc_kind, param_name, bound, err_json)) {
				SetJson(res, 422, err_json);
				finish();
				return;
			}

			// Constraint checks (LE/GE/…/min_length) — FastAPI Query(le=…) shape.
			if (spec && !from_default) {
				// For defaults we still apply constraints (default must be valid);
				// for request values always apply.
			}
			if (spec) {
				string cmsg, ctype;
				// bound.value may be accessed via BoundParameterData — use the Value we stored.
				// BoundParameterData holds Value in .value in DuckDB — check API.
				// We re-cast from raw for constraint checking to avoid depending on
				// BoundParameterData layout:
				Value constraint_val;
				if (expected.id() != LogicalTypeId::UNKNOWN && expected.id() != LogicalTypeId::VARCHAR) {
					string cerr;
					Value(raw).DefaultTryCastAs(expected, constraint_val, &cerr);
				} else {
					constraint_val = Value(raw);
				}
				if (!CheckParamConstraints(*spec, raw, constraint_val, cmsg, ctype)) {
					SetJson(res, 422, ValidationErrorJson(loc_kind, param_name, cmsg, ctype));
					finish();
					return;
				}
			}

			named_values[param_name] = bound;
			bound_raw[param_name] = {loc_kind, raw};
			(void)from_default;
		}

		auto result = prepared->Execute(named_values, false);
		if (result->HasError()) {
			// Client-input failures must never surface as 500.
			// - Conversion errors → 422 with recovered param name (not "_")
			// - LIMIT/OFFSET negative → empty 200 [] (FastAPI unconstrained int)
			// - Other binder/invalid-input from values → 422
			// - True handler bugs → 500 sanitized
			auto err = result->GetError();
			auto err_lower = StringUtil::Lower(err);
			bool conversion = StringUtil::Contains(err_lower, "conversion error") ||
			                  StringUtil::Contains(err_lower, "could not convert string");
			bool limit_neg = StringUtil::Contains(err_lower, "limit/offset cannot be negative") ||
			                 StringUtil::Contains(err_lower, "limit cannot be negative") ||
			                 StringUtil::Contains(err_lower, "offset cannot be negative");

			if (limit_neg) {
				// FastAPI returns [] for limit=-1 without ge constraint; never 500.
				fprintf(stderr, "quackapi: client limit/offset negative → []: %s\n", err.c_str());
				SetJson(res, 200, "[]");
			} else if (conversion) {
				fprintf(stderr, "quackapi: param conversion at execute: %s\n", err.c_str());
				string loc_kind = "query";
				string pname = "_";
				// Recover param name: match raw bound value against the error text.
				idx_t best_len = 0;
				for (auto &kv : bound_raw) {
					auto &raw = kv.second.second;
					if (raw.empty()) {
						continue;
					}
					if (StringUtil::Contains(err, raw) && raw.size() >= best_len) {
						best_len = raw.size();
						pname = kv.first;
						loc_kind = kv.second.first;
					}
				}
				// Fallback: single non-claims bound param.
				if (pname == "_" && bound_raw.size() == 1) {
					pname = bound_raw.begin()->first;
					loc_kind = bound_raw.begin()->second.first;
				}
				SetJson(res, 422,
				        ValidationErrorJson(loc_kind, pname, "Invalid input for parameter type", "type_error"));
			} else if (StringUtil::Contains(err_lower, "invalid input") ||
			           StringUtil::Contains(err_lower, "binder error") ||
			           StringUtil::Contains(err_lower, "out of range")) {
				// Likely client-driven; prefer 422 over Internal Server Error.
				fprintf(stderr, "quackapi: client-input execute error → 422: %s\n", err.c_str());
				string loc_kind = "query";
				string pname = "_";
				idx_t best_len = 0;
				for (auto &kv : bound_raw) {
					auto &raw = kv.second.second;
					if (!raw.empty() && StringUtil::Contains(err, raw) && raw.size() >= best_len) {
						best_len = raw.size();
						pname = kv.first;
						loc_kind = kv.second.first;
					}
				}
				SetJson(res, 422,
				        ValidationErrorJson(loc_kind, pname, "Invalid input for parameter", "value_error"));
			} else {
				SetInternalError(res, err);
			}
			finish();
			return;
		}

		auto &names = result->names;
		// Identify special response columns (FastAPI RedirectResponse / response cookies).
		// `location` → Location header; `set_cookie` / `set-cookie` → Set-Cookie.
		// These are stripped from the JSON/HTML/TEXT body.
		vector<bool> is_special(names.size(), false);
		vector<idx_t> data_cols;
		idx_t location_col = names.size(); // invalid sentinel
		idx_t set_cookie_col = names.size();
		for (idx_t c = 0; c < names.size(); c++) {
			auto lower = StringUtil::Lower(names[c]);
			if (lower == "location") {
				is_special[c] = true;
				location_col = c;
			} else if (lower == "set_cookie" || lower == "set-cookie") {
				is_special[c] = true;
				set_cookie_col = c;
			} else {
				data_cols.push_back(c);
			}
		}

		// Materialize rows once so we can apply headers then serialize body.
		struct RowVals {
			vector<Value> cols;
		};
		vector<RowVals> rows;
		while (true) {
			auto chunk = result->Fetch();
			if (!chunk || chunk->size() == 0) {
				break;
			}
			for (idx_t row = 0; row < chunk->size(); row++) {
				RowVals rv;
				rv.cols.resize(chunk->ColumnCount());
				for (idx_t col = 0; col < chunk->ColumnCount(); col++) {
					rv.cols[col] = chunk->GetValue(col, row);
				}
				rows.push_back(std::move(rv));
			}
		}

		// Apply Location / Set-Cookie from first (or each) row.
		string location_value;
		vector<string> set_cookie_values;
		for (auto &rv : rows) {
			if (location_col < rv.cols.size() && !rv.cols[location_col].IsNull() && location_value.empty()) {
				location_value = rv.cols[location_col].ToString();
			}
			if (set_cookie_col < rv.cols.size() && !rv.cols[set_cookie_col].IsNull()) {
				set_cookie_values.push_back(rv.cols[set_cookie_col].ToString());
			}
		}
		if (!location_value.empty()) {
			res.set_header("Location", location_value);
		}
		for (auto &cv : set_cookie_values) {
			// httplib Headers is multimap — set_header appends.
			res.set_header("Set-Cookie", cv);
		}

		// HTML/TEXT mode uses the single remaining data column name.
		vector<string> data_names;
		for (auto c : data_cols) {
			data_names.push_back(names[c]);
		}
		auto mode = ResponseModeFor(data_names);

		// No data columns: empty body (redirect / cookie-only responses).
		if (data_cols.empty()) {
			res.status = match.route.status;
			res.body.clear();
			// Drop Content-Type when there is no body.
			finish();
			return;
		}

		// HTML/TEXT mode: a single data column named `html`/`text` serves its raw
		// string value (e.g. SELECT tera_render(...) AS html). Multiple rows are
		// concatenated in order, so a query returning fragments streams a page.
		if (mode != ResponseMode::JSON) {
			string body;
			idx_t col = data_cols[0];
			for (auto &rv : rows) {
				if (col < rv.cols.size() && !rv.cols[col].IsNull()) {
					body += rv.cols[col].ToString();
				}
			}
			res.status = match.route.status;
			res.set_content(body, mode == ResponseMode::HTML ? "text/html; charset=utf-8"
			                                                  : "text/plain; charset=utf-8");
			// Keep body so Content-Length is correct; httplib omits the body for HEAD.
			finish();
			return;
		}

		// Serialize: JSON array of row objects, typed by the query's data columns.
		string body = "[";
		bool first_row = true;
		for (auto &rv : rows) {
			if (!first_row) {
				body += ",";
			}
			first_row = false;
			body += "{";
			bool first_col = true;
			for (auto col : data_cols) {
				if (!first_col) {
					body += ",";
				}
				first_col = false;
				body += "\"" + JsonEscape(names[col]) + "\":" + ValueToJson(rv.cols[col]);
			}
			body += "}";
		}
		body += "]";
		SetJson(res, match.route.status, body);
		// Keep body so Content-Length is correct; httplib omits the body for HEAD.
	} catch (std::exception &ex) {
		SetInternalError(res, ex.what());
	} catch (...) {
		SetInternalError(res, "unknown exception");
	}
	finish();
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
