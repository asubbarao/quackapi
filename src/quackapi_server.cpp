#include "quackapi_server.hpp"

#include "quackapi_logging.hpp"
#include "quackapi_request_binding.hpp"
#include "quackapi_response_encoding.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"

#include "quackapi_auth.hpp"
#include "quackapi_openapi.hpp"
#include "quackapi_limits.hpp"
#include "quackapi_middleware.hpp"
#include "quackapi_state.hpp"

#include "httplib.hpp"
#include "miniz_wrapper.hpp"
#include "zstd.h"
#include "quackapi_util.hpp"
#include "quackapi_validation.hpp"

namespace duckdb {

namespace {

//! Active connection for this httplib worker — set around process_request so a
//! matched CREATE ROUTE … TIMEOUT can extend SocketStream + SO_*TIMEO deadlines.
thread_local socket_t quackapi_tls_sock = INVALID_SOCKET;
thread_local duckdb_httplib::Stream *quackapi_tls_stream = nullptr;

//! httplib Server subclass: stash the live Stream/socket for per-route IO timeouts.
struct QuackapiHttplibServer : duckdb_httplib::Server {
	bool process_and_close_socket(socket_t sock) override {
		std::string remote_addr;
		int remote_port = 0;
		duckdb_httplib::detail::get_remote_ip_and_port(sock, remote_addr, remote_port);

		std::string local_addr;
		int local_port = 0;
		duckdb_httplib::detail::get_local_ip_and_port(sock, local_addr, local_port);

		auto ret = duckdb_httplib::detail::process_server_socket(
		    svr_sock_, sock, keep_alive_max_count_, keep_alive_timeout_sec_, read_timeout_sec_, read_timeout_usec_,
		    write_timeout_sec_, write_timeout_usec_,
		    [&](duckdb_httplib::Stream &strm, bool close_connection, bool &connection_closed) {
			    quackapi_tls_sock = sock;
			    quackapi_tls_stream = &strm;
			    auto ok = process_request(strm, remote_addr, remote_port, local_addr, local_port, close_connection,
			                              connection_closed, nullptr);
			    // Restore accept-time SO_* after handler+write (per-route TIMEOUT may
			    // have raised them). Next keep-alive SocketStream still uses serve defaults.
			    duckdb_httplib::detail::set_socket_opt_time(sock, SOL_SOCKET, SO_RCVTIMEO, read_timeout_sec_,
			                                                read_timeout_usec_);
			    duckdb_httplib::detail::set_socket_opt_time(sock, SOL_SOCKET, SO_SNDTIMEO, write_timeout_sec_,
			                                                write_timeout_usec_);
			    quackapi_tls_stream = nullptr;
			    quackapi_tls_sock = INVALID_SOCKET;
			    return ok;
		    });

		duckdb_httplib::detail::shutdown_socket(sock);
		duckdb_httplib::detail::close_socket(sock);
		return ret;
	}
};

//! Extend httplib read/write deadlines for the current request.
//! Sets SocketStream select timeouts (covers the response write after the
//! handler returns) and SO_RCVTIMEO/SO_SNDTIMEO on the accepted socket.
//! SO_* is restored to serve defaults in QuackapiHttplibServer after
//! process_request returns (keep-alive safe). Do not restore in a HandleRequest
//! destructor — that runs before httplib's write_response.
//! Route timeout introspection was removed; the socket deadlines remain local
//! to the active request.
void ApplyRouteIoTimeout(time_t timeout_sec) {
	if (timeout_sec <= 0) {
		return;
	}
	auto *stream = dynamic_cast<duckdb_httplib::detail::SocketStream *>(quackapi_tls_stream);
	if (stream) {
		stream->set_read_timeout(timeout_sec, 0);
		stream->set_write_timeout(timeout_sec, 0);
	}
	if (quackapi_tls_sock != INVALID_SOCKET) {
		duckdb_httplib::detail::set_socket_opt_time(quackapi_tls_sock, SOL_SOCKET, SO_RCVTIMEO, timeout_sec, 0);
		duckdb_httplib::detail::set_socket_opt_time(quackapi_tls_sock, SOL_SOCKET, SO_SNDTIMEO, timeout_sec, 0);
	}
}

//! Content-Encoding choice after Accept-Encoding negotiation.
enum class NegotiatedEncoding { IDENTITY, ZSTD, GZIP };

//! Parse one Accept-Encoding token's q-value (default 1.0 when omitted).
double ParseEncodingQ(const string &token) {
	// token forms: "zstd", "gzip;q=0.8", " *; q=0.1"
	auto parts = StringUtil::Split(token, ';');
	if (parts.size() < 2) {
		return 1.0;
	}
	for (idx_t i = 1; i < parts.size(); i++) {
		string p = parts[i];
		StringUtil::Trim(p);
		auto lower = StringUtil::Lower(p);
		if (StringUtil::StartsWith(lower, "q=")) {
			auto qstr = p.substr(2);
			StringUtil::Trim(qstr);
			try {
				auto q = std::stod(qstr);
				return q >= 0.0 && q <= 1.0 ? q : 0.0;
			} catch (...) {
				return 0.0;
			}
		}
	}
	return 1.0;
}

//! Negotiate Content-Encoding: choose the highest q-value, preferring zstd on ties.
//! Missing Accept-Encoding → identity. q=0 rejects a coding. `*` accepts any.
NegotiatedEncoding NegotiateContentEncoding(const duckdb_httplib::Request &req, QuackapiCompressionMode mode) {
	auto it = req.headers.find("Accept-Encoding");
	if (it == req.headers.end() || it->second.empty()) {
		return NegotiatedEncoding::IDENTITY;
	}
	// Case-insensitive multimap: first value wins (CollectHeaders pattern).
	string header = it->second;
	double zstd_q = -1.0;
	double gzip_q = -1.0;
	double identity_q = -1.0;
	double star_q = -1.0;
	auto tokens = StringUtil::Split(header, ',');
	for (auto &raw : tokens) {
		string tok = raw;
		StringUtil::Trim(tok);
		if (tok.empty()) {
			continue;
		}
		// Coding name is before optional parameters.
		string name = tok;
		auto semi = name.find(';');
		if (semi != string::npos) {
			name = name.substr(0, semi);
		}
		StringUtil::Trim(name);
		auto lower = StringUtil::Lower(name);
		double q = ParseEncodingQ(tok);
		if (lower == "zstd") {
			zstd_q = q;
		} else if (lower == "gzip" || lower == "x-gzip") {
			gzip_q = q;
		} else if (lower == "identity") {
			identity_q = q;
		} else if (lower == "*") {
			star_q = q;
		}
	}
	auto accepted = [](double explicit_q, double star) -> double {
		if (explicit_q >= 0.0) {
			return explicit_q;
		}
		if (star >= 0.0) {
			return star;
		}
		return 0.0;
	};
	double zstd_quality = accepted(zstd_q, star_q);
	double gzip_quality = accepted(gzip_q, star_q);
	if (mode == QuackapiCompressionMode::GZIP) {
		zstd_quality = 0.0;
	} else if (mode == QuackapiCompressionMode::ZSTD) {
		gzip_quality = 0.0;
	}
	if (zstd_quality > 0.0 && zstd_quality >= gzip_quality) {
		return NegotiatedEncoding::ZSTD;
	}
	if (gzip_quality > 0.0) {
		return NegotiatedEncoding::GZIP;
	}
	(void)identity_q;
	return NegotiatedEncoding::IDENTITY;
}

//! True for content types that are already compressed (skip re-compression).
bool IsAlreadyCompressedContentType(const string &content_type) {
	if (content_type.empty()) {
		return false;
	}
	string ct = StringUtil::Lower(content_type);
	auto semi = ct.find(';');
	if (semi != string::npos) {
		ct = ct.substr(0, semi);
	}
	StringUtil::Trim(ct);
	// SVG is text-like and compresses well; other images are already compressed.
	if (StringUtil::StartsWith(ct, "image/") && ct != "image/svg+xml") {
		return true;
	}
	if (StringUtil::StartsWith(ct, "audio/") || StringUtil::StartsWith(ct, "video/")) {
		return true;
	}
	static const char *kCompressed[] = {
	    "application/gzip",
	    "application/x-gzip",
	    "application/zip",
	    "application/x-zip-compressed",
	    "application/zstd",
	    "application/x-zstd",
	    "application/brotli",
	    "application/x-brotli",
	    "application/x-compress",
	    "application/x-xz",
	    "application/x-rar-compressed",
	    "application/wasm",
	    "font/woff",
	    "font/woff2",
	    "application/font-woff",
	    "application/font-woff2",
	};
	for (auto *t : kCompressed) {
		if (ct == t) {
			return true;
		}
	}
	return false;
}

//! Compress body with DuckDB-bundled zstd (duckdb_zstd). Returns false on failure.
bool CompressZstd(const string &input, string &output) {
	size_t bound = duckdb_zstd::ZSTD_compressBound(input.size());
	if (duckdb_zstd::ZSTD_isError(bound)) {
		return false;
	}
	output.resize(bound);
	auto written = duckdb_zstd::ZSTD_compress(&output[0], bound, input.data(), input.size(),
	                                          /*level=*/3);
	if (duckdb_zstd::ZSTD_isError(written)) {
		return false;
	}
	output.resize(written);
	return true;
}

//! Compress body with DuckDB-bundled miniz gzip wrapper. Returns false on failure.
bool CompressGzip(const string &input, string &output) {
	try {
		MiniZStream s;
		size_t out_size = MiniZStream::MaxCompressedLength(input.size());
		output.resize(out_size);
		s.Compress(input.data(), input.size(), &output[0], &out_size);
		output.resize(out_size);
		return true;
	} catch (...) {
		return false;
	}
}

//! FastAPI-shaped validation error body (loc = [kind, name]).
string ValidationErrorJson(const string &loc_kind, const string &param_name, const string &msg, const string &type) {
	return "{\"detail\":[{\"loc\":[\"" + QuackapiJsonEscape(loc_kind) + "\",\"" + QuackapiJsonEscape(param_name) +
	       "\"],\"msg\":\"" + QuackapiJsonEscape(msg) + "\",\"type\":\"" + QuackapiJsonEscape(type) + "\"}]}";
}

string ValidationLocJson(const string &loc_kind, const string &param_name) {
	return "[\"" + QuackapiJsonEscape(loc_kind) + "\",\"" + QuackapiJsonEscape(param_name) + "\"]";
}

//! FastAPI-shaped body-only validation error (loc = ["body"]).
string ValidationErrorJsonBody(const string &msg, const string &type) {
	return "{\"detail\":[{\"loc\":[\"body\"],\"msg\":\"" + QuackapiJsonEscape(msg) + "\",\"type\":\"" +
	       QuackapiJsonEscape(type) + "\"}]}";
}

//! httplib Headers/Params → JSON object. Function template, not a generic lambda
//! (C++14), so the extension stays on DuckDB's C++11 dialect.
template <class PairRange>
string PairsToJsonObject(const PairRange &pairs) {
	string json = "{";
	bool first = true;
	for (const auto &pair : pairs) {
		if (!first) {
			json += ",";
		}
		first = false;
		json += "\"" + QuackapiJsonEscape(pair.first) + "\":\"" + QuackapiJsonEscape(pair.second) + "\"";
	}
	return json + "}";
}

//! The json_schema extension includes an RFC 6901 pointer in failures (for
//! example, "At /items/0/qty: ..."). Surface it as FastAPI's typed loc array
//! instead of losing the field path behind a synthetic _schema member.
string ValidationErrorJsonSchema(Connection &con, const string &msg, const string &type, const string &raw_body) {
	const string markers[] = {"At /", "at /"};
	for (auto &marker : markers) {
		auto start_marker = msg.find(marker);
		if (start_marker == string::npos) {
			continue;
		}
		auto start = start_marker + marker.size() - 1; // retain the leading '/'
		// json_schema phrases a failing location as "At /path of <value>";
		// older releases use a colon or newline after the pointer instead.
		auto end = msg.find(" of ", start);
		auto punctuation_end = msg.find_first_of(":\n\r", start);
		if (end == string::npos || (punctuation_end != string::npos && punctuation_end < end)) {
			end = punctuation_end;
		}
		if (end == string::npos) {
			end = msg.size();
		}
		while (end > start && StringUtil::CharacterIsSpace(msg[end - 1])) {
			end--;
		}
		if (end > start) {
			return QuackapiValidationErrorsJson(
			    {{QuackapiValidationBodyPointerLoc(con, msg.substr(start, end - start), raw_body), msg, type}});
		}
	}
	return ValidationErrorJson("body", "_schema", msg, type);
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

//! Validate body against BODY SCHEMA using community json_schema extension.
//! json_schema_validate returns true on pass and THROWS on fail — wrap with try().
bool ValidateBodySchema(Connection &con, const string &schema, const string &raw_body, string &err_json) {
	// CREATE ROUTE resolves this dependency before requests start. LOAD is
	// idempotent on the database instance; never install from a request path.
	auto load = con.Query("LOAD json_schema");
	if (load->HasError()) {
		fprintf(stderr, "quackapi: json_schema unavailable: %s\n", load->GetError().c_str());
		err_json = ValidationErrorJsonBody("json_schema extension unavailable", "value_error");
		return false;
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
	err_json = ValidationErrorJsonSchema(con, msg, "value_error", raw_body);
	return false;
}

//! Run DuckDB's native JSON transform in strict mode. This is deliberately
//! independent of the optional json_schema extension: a BODY TYPE declaration
//! is a DuckDB type structure and validates/coerces through the engine itself.
bool TransformTypedBody(Connection &con, const string &body_type, const string &raw_body, Value &out,
                        string &err_json) {
	auto result = con.Query("SELECT try(json_transform_strict(?::JSON, ?::JSON))", Value(raw_body), Value(body_type));
	if (!result->HasError()) {
		auto chunk = result->Fetch();
		if (chunk && chunk->size() > 0 && !chunk->GetValue(0, 0).IsNull()) {
			out = chunk->GetValue(0, 0);
			return true;
		}
	}

	string msg = "Body type validation failed";
	auto strict = con.Query("SELECT json_transform_strict(?::JSON, ?::JSON)", Value(raw_body), Value(body_type));
	if (strict->HasError()) {
		msg = strict->GetError();
		const string prefixes[] = {"Invalid Input Error: ", "Invalid Error: ", "Binder Error: "};
		for (auto &prefix : prefixes) {
			if (StringUtil::StartsWith(msg, prefix)) {
				msg = msg.substr(prefix.size());
				break;
			}
		}
	}
	err_json = ValidationErrorJsonBody(msg, "type_error");
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
		msg = StringUtil::Format("String should have at least %llu characters", (unsigned long long)spec.min_length);
		err_type = "string_too_short";
		return false;
	}
	if (spec.has_max_length && raw.size() > spec.max_length) {
		msg = StringUtil::Format("String should have at most %llu characters", (unsigned long long)spec.max_length);
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

//! Mark quoted text and comments before looking for $params. The DuckDB lexer
//! owns SQL quoting rules, including dollar-quoted bodies, so route body
//! detection cannot mistake '$body' in a literal or comment for a request
//! parameter.
vector<bool> ProtectedSqlText(const string &sql) {
	vector<bool> protected_text(sql.size(), false);
	auto tokens = Parser::Tokenize(sql);
	for (idx_t t = 0; t < tokens.size(); t++) {
		auto &token = tokens[t];
		if (token.type != SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT &&
		    token.type != SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT &&
		    !(token.start < sql.size() && sql[token.start] == '"')) {
			continue;
		}
		auto end = t + 1 < tokens.size() ? tokens[t + 1].start : sql.size();
		for (idx_t p = token.start; p < end && p < sql.size(); p++) {
			protected_text[p] = true;
		}
	}
	return protected_text;
}

//! Resolve the native DuckDB type represented by a BODY TYPE declaration.
//! This lets route SQL use $body.items directly: the server inserts the cast
//! from the one declaration before DuckDB prepares the handler.
bool ResolveTypedBodySqlType(Connection &con, const string &body_type, string &sql_type, string &error) {
	auto first = body_type.find_first_not_of(" \t\n\r");
	string sample = "null";
	if (first != string::npos && body_type[first] == '{') {
		sample = "{}";
	} else if (first != string::npos && body_type[first] == '[') {
		sample = "[]";
	}
	auto result = con.Query("SELECT typeof(json_transform(?::JSON, ?::JSON))", Value(sample), Value(body_type));
	if (result->HasError()) {
		error = result->GetError();
		return false;
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0 || chunk->GetValue(0, 0).IsNull()) {
		error = "BODY TYPE does not describe a DuckDB value type";
		return false;
	}
	sql_type = chunk->GetValue(0, 0).GetValue<string>();
	return true;
}

bool IsBodyParameterAt(const string &sql, const vector<bool> &protected_text, idx_t index) {
	if (index + 5 > sql.size() || protected_text[index] || sql[index] != '$') {
		return false;
	}
	if (StringUtil::Lower(sql.substr(index + 1, 4)) != "body") {
		return false;
	}
	if (index + 5 < sql.size()) {
		auto next = sql[index + 5];
		if ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') || (next >= '0' && next <= '9') ||
		    next == '_') {
			return false;
		}
	}
	return true;
}

string RewriteTypedBodyParameter(const string &sql, const string &sql_type) {
	auto protected_text = ProtectedSqlText(sql);
	string rewritten;
	rewritten.reserve(sql.size() + sql_type.size());
	for (idx_t i = 0; i < sql.size();) {
		if (IsBodyParameterAt(sql, protected_text, i)) {
			// Preserve a developer-supplied explicit cast for compatibility.
			if (i + 6 < sql.size() && sql[i + 5] == ':' && sql[i + 6] == ':') {
				rewritten += sql.substr(i, 5);
			} else {
				rewritten += "($body::" + sql_type + ")";
			}
			i += 5;
			continue;
		}
		rewritten += sql[i++];
	}
	return rewritten;
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
		if (ps.size() >= 2 && ps.front() == '{' && ps.back() == '}' && ps.substr(1, ps.size() - 2) == param_name) {
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

static constexpr const char *QUACKAPI_STATIC_LOG_MARKER = "X-Quackapi-Static-Access-Log";
static constexpr const char *QUACKAPI_STATIC_LOG_STARTED = "X-Quackapi-Static-Started";
static constexpr const char *QUACKAPI_STATIC_LOG_RECEIVED = "X-Quackapi-Static-Received";

string SanitizeClientRequestId(const string &s);
static void MakeTraceContext(const string &traceparent, string &trace_id, string &span_id, string &parent_span_id,
                             bool &sampled);
static QuackapiRequestRecord MakeRequestRecord(const duckdb_httplib::Request &req, const duckdb_httplib::Response &res,
                                               const string &request_id, const string &route_name,
                                               const string &route_path, double latency_ms, int64_t received_at_micros,
                                               double sql_prepare_ms, double sql_execute_ms, int64_t rows_out,
                                               const string &trace_id, const string &span_id,
                                               const string &parent_span_id, bool sampled,
                                               const QuackapiServeOptions &options);

QuackapiHttpServer::QuackapiHttpServer(DatabaseInstance &db, const string &host_p, int port_p,
                                       const QuackapiServeOptions &opts, bool bind_and_listen)
    : db_ptr(db.shared_from_this()), host(host_p), port(port_p), cors_origins(opts.cors_origins), options(opts),
      started_at(std::chrono::steady_clock::now()), compression(opts.compression),
      compression_min_bytes(opts.compression_min_bytes) {
	// JSON is a runtime dependency for typed request bodies and validation. Load
	// an available copy once the database is initialized; never install it here.
	if (!db.ExtensionIsLoaded("json") && !ExtensionHelper::TryAutoLoadAvailableExtension(db, "json")) {
		throw IOException("QuackAPI requires the DuckDB JSON extension; load json before serving requests");
	}
	// In-process only (quackapi_request): no TCP server object, no bind.
	if (!bind_and_listen) {
		is_running.store(false);
		return;
	}

	server = make_uniq<QuackapiHttplibServer>();

	// Static files (FastAPI StaticFiles equivalent). httplib checks file
	// requests before route handlers, so API routes always win over files.
	if (!opts.static_dir.empty() && !server->set_mount_point("/", opts.static_dir)) {
		throw IOException("quackapi: static_dir \"%s\" is not a directory", opts.static_dir);
	}
	server->set_file_request_handler([this](const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
		auto db = db_ptr.lock();
		if (!db) {
			return;
		}
		auto request_id = SanitizeClientRequestId(req.get_header_value("X-Request-ID"));
		if (request_id.empty()) {
			request_id = NextRequestId(*db);
		}
		res.set_header("X-Request-ID", request_id);
		res.set_header(QUACKAPI_STATIC_LOG_MARKER, "1");
		res.set_header(QUACKAPI_STATIC_LOG_STARTED, std::to_string(static_cast<int64_t>(
		                                                std::chrono::steady_clock::now().time_since_epoch().count())));
		res.set_header(QUACKAPI_STATIC_LOG_RECEIVED, std::to_string(Timestamp::GetCurrentTimestamp().value));
	});
	server->set_post_routing_handler([this](const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
		auto marker = res.headers.find(QUACKAPI_STATIC_LOG_MARKER);
		if (marker == res.headers.end()) {
			return;
		}
		res.headers.erase(marker);
		auto started = res.headers.find(QUACKAPI_STATIC_LOG_STARTED);
		auto received = res.headers.find(QUACKAPI_STATIC_LOG_RECEIVED);
		int64_t received_at = Timestamp::GetCurrentTimestamp().value;
		if (received != res.headers.end()) {
			try {
				received_at = std::stoll(received->second);
			} catch (...) {
			}
			res.headers.erase(received);
		}
		double latency_ms = 0;
		if (started != res.headers.end()) {
			try {
				const auto started_ticks = std::stoll(started->second);
				latency_ms =
				    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch() -
				                                              std::chrono::steady_clock::duration(started_ticks))
				        .count();
			} catch (...) {
			}
			res.headers.erase(started);
		}
		string trace_id, span_id, parent_span_id;
		bool sampled;
		MakeTraceContext(req.get_header_value("traceparent"), trace_id, span_id, parent_span_id, sampled);
		EmitAccessLog(MakeRequestRecord(req, res, res.get_header_value("X-Request-ID"), "static", string(), latency_ms,
		                                received_at, -1, -1, -1, trace_id, span_id, parent_span_id, sampled, options));
	});

	// Transport defaults (overridable via serve opts) — correct-by-default for servers.
	int32_t workers =
	    opts.worker_threads > 0 ? opts.worker_threads : static_cast<int32_t>(QUACKAPI_DEFAULT_WORKER_THREADS);
	const auto max_pending = opts.max_pending_requests;
	server->new_task_queue = [workers, max_pending] {
		return new duckdb_httplib::ThreadPool(static_cast<size_t>(workers), static_cast<size_t>(max_pending));
	};
	server->set_keep_alive_max_count(opts.keep_alive_max_count > 0 ? static_cast<size_t>(opts.keep_alive_max_count)
	                                                               : QUACKAPI_DEFAULT_KEEP_ALIVE_MAX);
	server->set_keep_alive_timeout(opts.keep_alive_timeout_sec > 0 ? static_cast<time_t>(opts.keep_alive_timeout_sec)
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

void QuackapiHttpServer::Dispatch(const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
	HandleRequest(req, res);
}

void QuackapiInProcessRequest(DatabaseInstance &db, const string &method, const string &path_in, const string &body,
                              int &status_out, string &body_out, string &content_type_out,
                              const unordered_map<string, string> *req_headers,
                              unordered_map<string, string> *headers_out, const QuackapiServeOptions *request_options) {
	// Quiet defaults for SQL tests unless request_options explicitly enables logging.
	QuackapiServeOptions opts;
	if (request_options) {
		opts = *request_options;
	} else {
		opts.access_log = false;
	}
	opts.compression = QuackapiCompressionMode::OFF;
	opts.health_routes = true;
	// No TCP — Dispatch only.
	QuackapiHttpServer server(db, "127.0.0.1", 0, opts, /*bind_and_listen=*/false);

	duckdb_httplib::Request req;
	req.method = StringUtil::Upper(method);
	string path = path_in;
	string query;
	auto qpos = path.find('?');
	if (qpos != string::npos) {
		query = path.substr(qpos + 1);
		path = path.substr(0, qpos);
	}
	if (path.empty()) {
		path = "/";
	}
	req.path = path;
	req.target = path_in;
	req.version = "HTTP/1.1";
	req.remote_addr = "127.0.0.1";
	req.remote_port = 0;
	req.local_addr = "127.0.0.1";
	req.local_port = 0;
	ParseQueryStringIntoParams(query, req.params);
	if (req_headers) {
		for (auto &kv : *req_headers) {
			req.set_header(kv.first, kv.second);
		}
	}
	if (!body.empty()) {
		req.body = body;
		if (!req.has_header("Content-Type")) {
			req.set_header("Content-Type", "application/json");
		}
		req.set_header("Content-Length", std::to_string(body.size()));
	}

	duckdb_httplib::Response res;
	server.Dispatch(req, res);

	status_out = res.status;
	body_out = res.body;
	content_type_out = res.get_header_value("Content-Type");
	if (headers_out) {
		headers_out->clear();
		for (auto &h : res.headers) {
			// First value wins for MAP (SQLLogic asserts one value).
			if (headers_out->find(h.first) == headers_out->end()) {
				(*headers_out)[h.first] = h.second;
			}
		}
	}
}

string QuackapiHttpServer::NextRequestId(DatabaseInstance &db) {
	// Always C++ uuidv7 — never SELECT per request (tsid() was a multi-ms tax
	// on every route including /hello). request_id_source is informational.
	(void)db;
	return UUID::ToString(UUIDv7::GenerateRandomUUID());
}

//! Client-supplied X-Request-ID: strip controls / non-printable, cap at 128 chars.
//! Empty result → caller mints uuidv7.
string SanitizeClientRequestId(const string &s) {
	string out;
	out.reserve(std::min<size_t>(s.size(), 128));
	for (unsigned char c : s) {
		if (c >= 0x21 && c <= 0x7e) {
			out.push_back(static_cast<char>(c));
			if (out.size() >= 128) {
				break;
			}
		}
	}
	return out;
}

static bool IsHexString(const string &value) {
	for (auto c : value) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
			return false;
		}
	}
	return true;
}

static bool IsAllZero(const string &value) {
	for (auto c : value) {
		if (c != '0') {
			return false;
		}
	}
	return true;
}

static string RandomTraceHex(idx_t length) {
	auto raw = StringUtil::Replace(UUID::ToString(UUIDv7::GenerateRandomUUID()), "-", "");
	return length == 16 ? raw.substr(16, length) : raw.substr(0, length);
}

static void MakeTraceContext(const string &traceparent, string &trace_id, string &span_id, string &parent_span_id,
                             bool &sampled) {
	trace_id = RandomTraceHex(32);
	span_id = RandomTraceHex(16);
	parent_span_id.clear();
	sampled = true;
	if (traceparent.size() == 55 && traceparent[2] == '-' && traceparent[35] == '-' && traceparent[52] == '-' &&
	    traceparent.compare(0, 2, "00") == 0) {
		auto inbound_trace = traceparent.substr(3, 32);
		auto inbound_parent = traceparent.substr(36, 16);
		auto flags = traceparent.substr(53, 2);
		if (IsHexString(inbound_trace) && IsHexString(inbound_parent) && IsHexString(flags) &&
		    !IsAllZero(inbound_trace) && !IsAllZero(inbound_parent)) {
			trace_id = StringUtil::Lower(inbound_trace);
			parent_span_id = StringUtil::Lower(inbound_parent);
			sampled = (std::strtoul(flags.c_str(), nullptr, 16) & 1U) != 0;
		}
	}
}

static string ErrorMessageFromResponse(const string &body) {
	static constexpr const char *prefix = "{\"detail\":\"";
	if (!StringUtil::StartsWith(body, prefix)) {
		return body.substr(0, 512);
	}
	auto start = strlen(prefix);
	for (idx_t i = start; i < body.size(); i++) {
		if (body[i] == '"' && (i == start || body[i - 1] != '\\')) {
			return body.substr(start, std::min<idx_t>(i - start, 512));
		}
	}
	return body.substr(start, 512);
}

static string ErrorTypeForStatus(int status) {
	switch (status) {
	case 502:
		return "upstream_error";
	case 503:
		return "service_unavailable";
	case 504:
		return "timeout";
	case 507:
		return "response_too_large";
	default:
		return status >= 500 ? "internal_error" : "http_error";
	}
}

static int64_t ResponseBytes(const duckdb_httplib::Response &res) {
	size_t bytes = res.body.size();
	// Prefer Content-Length when set; body may be empty for HEAD.
	auto cl = res.headers.find("Content-Length");
	if (cl != res.headers.end()) {
		try {
			bytes = static_cast<size_t>(std::stoull(cl->second));
		} catch (...) {
		}
	}
	return static_cast<int64_t>(bytes);
}

static bool IsDeniedHeaderName(const string &name) {
	return name == "authorization" || name == "proxy-authorization" || name == "cookie" || name == "set-cookie" ||
	       name == "x-api-key" || StringUtil::Contains(name, "token") || StringUtil::Contains(name, "secret") ||
	       StringUtil::Contains(name, "password");
}

static unordered_map<string, string> RedactedHeaders(const duckdb_httplib::Request &req, const string &allowlist) {
	vector<string> allowed;
	for (auto name : StringUtil::Split(allowlist, ',')) {
		StringUtil::Trim(name);
		name = StringUtil::Lower(name);
		if (!name.empty()) {
			allowed.push_back(std::move(name));
		}
	}
	unordered_map<string, string> result;
	for (auto &header : req.headers) {
		auto name = StringUtil::Lower(header.first);
		bool is_allowed = false;
		for (auto &candidate : allowed) {
			if (name == candidate) {
				is_allowed = true;
				break;
			}
		}
		if (is_allowed && result.find(name) == result.end()) {
			result.emplace(name, IsDeniedHeaderName(name) ? "[REDACTED]" : header.second);
		}
	}
	return result;
}

static bool IsDeniedQueryKey(const string &raw_key) {
	auto key = StringUtil::Lower(duckdb_httplib::decode_query_component(raw_key, true));
	static constexpr const char *DENIED_QUERY_KEY_PARTS[] = {"token", "key",  "secret", "password",
	                                                         "sig",   "code", "auth"};
	for (auto part : DENIED_QUERY_KEY_PARTS) {
		if (StringUtil::Contains(key, part)) {
			return true;
		}
	}
	return false;
}

static string RedactRawQuery(const string &target) {
	auto query_start = target.find('?');
	if (query_start == string::npos) {
		return string();
	}
	auto raw_query = target.substr(query_start + 1);
	string redacted;
	redacted.reserve(raw_query.size());
	idx_t start = 0;
	while (start <= raw_query.size()) {
		if (start > 0) {
			redacted.push_back('&');
		}
		auto end = raw_query.find('&', start);
		if (end == string::npos) {
			end = raw_query.size();
		}
		auto pair = raw_query.substr(start, end - start);
		auto equals = pair.find('=');
		auto raw_key = pair.substr(0, equals);
		redacted += raw_key;
		if (IsDeniedQueryKey(raw_key)) {
			redacted += "=[REDACTED]";
		} else if (equals != string::npos) {
			redacted += pair.substr(equals);
		}
		if (end == raw_query.size()) {
			break;
		}
		start = end + 1;
	}
	return redacted;
}

static QuackapiRequestRecord MakeRequestRecord(const duckdb_httplib::Request &req, const duckdb_httplib::Response &res,
                                               const string &request_id, const string &route_name,
                                               const string &route_path, double latency_ms, int64_t received_at_micros,
                                               double sql_prepare_ms, double sql_execute_ms, int64_t rows_out,
                                               const string &trace_id, const string &span_id,
                                               const string &parent_span_id, bool sampled,
                                               const QuackapiServeOptions &options) {
	QuackapiRequestRecord entry;
	entry.request_id = request_id;
	entry.received_at_micros = received_at_micros;
	entry.method = req.method;
	entry.path = req.path;
	entry.route_name = route_name;
	entry.route_path = route_path;
	entry.status = res.status;
	entry.duration_ms = latency_ms;
	entry.sql_prepare_ms = sql_prepare_ms;
	entry.sql_execute_ms = sql_execute_ms;
	entry.rows_out = rows_out;
	entry.bytes_in = static_cast<int64_t>(req.body.size());
	entry.bytes_out = ResponseBytes(res);
	entry.client_ip = req.remote_addr;
	entry.user_agent = req.get_header_value("User-Agent").substr(0, 256);
	entry.http_version = req.version;
	if (res.status >= 400) {
		entry.error_type = ErrorTypeForStatus(res.status);
		if (res.status >= 500) {
			entry.error_message = ErrorMessageFromResponse(res.body);
		}
	}
	entry.trace_id = trace_id;
	entry.span_id = span_id;
	entry.parent_span_id = parent_span_id;
	entry.sampled = sampled;
	entry.headers_logged = !options.log_headers.empty();
	if (entry.headers_logged) {
		entry.headers = RedactedHeaders(req, options.log_headers);
	}
	entry.query_logged = options.log_query;
	if (entry.query_logged) {
		entry.query = RedactRawQuery(req.target);
	}
	return entry;
}

void QuackapiHttpServer::EmitAccessLog(const QuackapiRequestRecord &entry) {
	if (!options.access_log) {
		return;
	}
	LogLevel level;
	bool emit = false;
	switch (options.log_level) {
	case QuackapiLogLevel::DEBUG_LEVEL:
	case QuackapiLogLevel::INFO:
		emit = true;
		break;
	case QuackapiLogLevel::WARN:
		emit = entry.status >= 400 || entry.duration_ms >= static_cast<double>(options.slow_request_ms);
		break;
	case QuackapiLogLevel::ERROR_LEVEL:
		emit = entry.status >= 500;
		break;
	case QuackapiLogLevel::SILENT:
		break;
	}
	if (!emit) {
		return;
	}
	if (entry.status >= 500) {
		level = LogLevel::LOG_ERROR;
	} else if (entry.status >= 400 || entry.duration_ms >= static_cast<double>(options.slow_request_ms)) {
		level = LogLevel::LOG_WARNING;
	} else if (options.log_level == QuackapiLogLevel::DEBUG_LEVEL) {
		level = LogLevel::LOG_DEBUG;
	} else {
		level = LogLevel::LOG_INFO;
	}
	auto db = db_ptr.lock();
	if (db) {
		EmitQuackapiAccessLog(*db, entry, level);
	}
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
		// Emit the literal wildcard — never echo the request Origin (including
		// the literal string "null", sent by browsers for file://, sandboxed
		// iframes, and opaque origins). Echoing turns a same-value-for-everyone
		// wildcard into an attacker-controlled reflection, which browsers will
		// also reject combined with credentialed requests anyway (fuzz catalog
		// P0-sec "CORS cors_origins='*' reflects any Origin incl. null").
		allow_origin = "*";
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

void QuackapiHttpServer::MaybeCompressResponse(const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
	if (compression == QuackapiCompressionMode::OFF) {
		return;
	}
	// No entity / no point compressing tiny payloads.
	if (res.body.empty() || res.body.size() < compression_min_bytes) {
		return;
	}
	// 204/304 must not carry a body; leave alone.
	if (res.status == 204 || res.status == 304) {
		return;
	}
	// Already encoded (shouldn't happen on our paths).
	if (res.has_header("Content-Encoding")) {
		return;
	}
	string content_type = res.get_header_value("Content-Type");
	if (IsAlreadyCompressedContentType(content_type)) {
		return;
	}
	// This response was eligible for negotiation, even when the client selects
	// identity. Keep caches from sharing the identity representation with an
	// encoded representation.
	res.set_header("Vary", "Accept-Encoding");
	auto encoding = NegotiateContentEncoding(req, compression);
	if (encoding == NegotiatedEncoding::IDENTITY) {
		return;
	}
	string compressed;
	const char *encoding_name = nullptr;
	if (encoding == NegotiatedEncoding::ZSTD) {
		if (!CompressZstd(res.body, compressed)) {
			return;
		}
		encoding_name = "zstd";
	} else if (encoding == NegotiatedEncoding::GZIP) {
		if (!CompressGzip(res.body, compressed)) {
			return;
		}
		encoding_name = "gzip";
	} else {
		return;
	}
	// Only swap body if compression actually shrank it.
	if (compressed.size() >= res.body.size()) {
		return;
	}
	res.body = std::move(compressed);
	res.set_header("Content-Encoding", encoding_name);
	// Content-Length is recomputed by httplib from body; strip any stale value.
	res.headers.erase("Content-Length");
}

// In-process sliding fixed-window rate limiter (no Redis). Keyed by
// route name + client key + floor(now / window). thread-safe.
// CREATE OR REPLACE / DROP of a route clears that route's buckets so a new
// registration does not inherit a spent window from the prior definition.
bool QuackapiState::AllowRateLimit(const string &route, const string &client, int limit, int per_sec,
                                   int &retry_after_sec) {
	const auto now = std::chrono::steady_clock::now();
	const string key = std::to_string(route.size()) + ":" + route + "|" + client;
	retry_after_sec = std::max(1, per_sec);
	std::lock_guard<std::mutex> guard(rate_limit_mutex);
	auto found = rate_limit_entries.find(key);
	if (found == rate_limit_entries.end()) {
		if (rate_limit_entries.size() >= 100000) {
			for (auto it = rate_limit_entries.begin(); it != rate_limit_entries.end();) {
				if (it->second.expires <= now) {
					it = rate_limit_entries.erase(it);
				} else {
					++it;
				}
			}
		}
		// Fail closed rather than retaining an unbounded number of live identities.
		if (rate_limit_entries.size() >= 100000) {
			return false;
		}
		RateLimitEntry entry;
		entry.expires = now + std::chrono::seconds(per_sec);
		found = rate_limit_entries.emplace(key, entry).first;
	}
	auto &entry = found->second;
	if (entry.expires <= now) {
		entry.count = 0;
		entry.expires = now + std::chrono::seconds(per_sec);
	}
	retry_after_sec =
	    std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::seconds>(entry.expires - now).count() + 1);
	if (entry.count >= limit) {
		return false;
	}
	entry.count++;
	return true;
}

void QuackapiState::ClearRouteRateLimit(const string &route) {
	const string prefix = std::to_string(route.size()) + ":" + route + "|";
	std::lock_guard<std::mutex> guard(rate_limit_mutex);
	for (auto it = rate_limit_entries.begin(); it != rate_limit_entries.end();) {
		if (StringUtil::StartsWith(it->first, prefix)) {
			it = rate_limit_entries.erase(it);
		} else {
			++it;
		}
	}
}

static string RateLimitClientKey(DatabaseInstance &db, const duckdb_httplib::Request &req, const string &by,
                                 const QuackapiRoute &route, const QuackapiAuthResult &verified) {
	if (by == "token" || by == "key") {
		// Only verified identities can select their own bucket. An unauthenticated
		// caller cannot evade an IP quota by inventing arbitrary token strings.
		QuackapiAuth scheme;
		if (!route.require_auth.empty() && verified.ok && QuackapiState::Get(db).GetAuth(route.require_auth, scheme)) {
			auto sub = verified.claims.find("sub");
			if (sub != verified.claims.end() && !sub->second.empty()) {
				return "sub:" + QuackapiSha256(route.require_auth + ":" + sub->second);
			}
			return "tok:" + QuackapiSha256(ExtractAuthString(scheme, CollectHeaders(req)));
		}
		// Fall back to IP when no credential presented.
	}
	string ip = req.remote_addr.empty() ? string("unknown") : req.remote_addr;
	return "ip:" + ip;
}

void QuackapiHttpServer::HandleRequest(const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
	// Always attach CORS headers when configured (including error responses).
	// Stamp X-Request-ID + emit structured access log on every exit path.
	// Applied once at the end via a small RAII-ish pattern: call ApplyCors on
	// every exit path is error-prone, so we apply after handling via a lambda
	// wrapper below. Compression runs after CORS so both sets of headers land
	// on the response; the access log is emitted last so its byte count
	// reflects the (possibly compressed) final body.
	const auto t0 = std::chrono::steady_clock::now();
	const auto received_at_micros = Timestamp::GetCurrentTimestamp().value;
	string request_id; // filled once db is available; may be empty on 503 shutdown
	string route_name;
	string route_path;
	string trace_id, span_id, parent_span_id;
	bool sampled = true;
	double sql_prepare_ms = -1;
	double sql_execute_ms = -1;
	int64_t rows_out = -1;
	MakeTraceContext(req.get_header_value("traceparent"), trace_id, span_id, parent_span_id, sampled);

	std::function<void()> after_middleware;
	auto finish = [&]() {
		if (after_middleware) {
			auto run_after = std::move(after_middleware);
			after_middleware = nullptr;
			run_after();
		}
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >=
		    options.query_timeout_ms) {
			res.headers.erase("Content-Encoding");
			SetJson(res, 504, "{\"detail\":\"Query execution deadline exceeded\"}");
		} else if (res.body.size() > static_cast<size_t>(options.max_response_bytes)) {
			res.headers.erase("Content-Encoding");
			SetJson(res, 507, "{\"detail\":\"Response exceeds configured byte limit\"}");
		}
		if (!request_id.empty()) {
			res.set_header("X-Request-ID", request_id);
		}
		ApplyCorsHeaders(req, res);
		MaybeCompressResponse(req, res);
		auto t1 = std::chrono::steady_clock::now();
		double latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		EmitAccessLog(MakeRequestRecord(req, res, request_id.empty() ? string("-") : request_id, route_name, route_path,
		                                latency_ms, received_at_micros, sql_prepare_ms, sql_execute_ms, rows_out,
		                                trace_id, span_id, parent_span_id, sampled, options));
	};

	auto db = db_ptr.lock();
	if (!db) {
		SetJson(res, 503, "{\"detail\":\"database shutting down\"}");
		finish();
		return;
	}
	// Honor inbound X-Request-ID when usable; else mint uuidv7 (always set header).
	// Lookup is case-insensitive (httplib Headers). Strip controls + cap 128.
	{
		auto client_rid = SanitizeClientRequestId(req.get_header_value("X-Request-ID"));
		if (!client_rid.empty()) {
			request_id = std::move(client_rid);
		} else {
			request_id = NextRequestId(*db);
		}
	}

	// Built-in health routes (also registered in quackapi_routes() for listing).
	// Liveness: process accepting HTTP. Readiness: DB handle + version + uptime.
	// Exact-path only: the trailing-slash form falls through to normal route
	// matching below, which 307-redirects to the registered "/health"/"/healthz"
	// route (Starlette redirect_slashes policy — see trailing_slash.test.sh).
	if (options.health_routes && (req.method == "GET" || req.method == "HEAD")) {
		if (req.path == "/health") {
			route_name = "health";
			route_path = "/health";
			// Object body (not row-array) — standard k8s/load-balancer shape.
			SetJson(res, 200, "{\"status\":\"ok\"}");
			finish();
			return;
		}
		if (req.path == "/healthz") {
			route_name = "healthz";
			route_path = "/healthz";
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
			    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at).count();
			if (ready) {
				SetJson(
				    res, 200,
				    StringUtil::Format(
				        "{\"status\":\"ok\",\"version\":\"%s\",\"uptime_sec\":%lld,"
				        "\"request_id_source\":\"%s\"}",
				        QuackapiJsonEscape(version), (long long)uptime_sec,
				        QuackapiJsonEscape(options.request_id_source.empty() ? "uuidv7" : options.request_id_source)));
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
			route_name = "openapi";
			route_path = "/openapi.json";
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
			route_name = "docs";
			route_path = "/docs";
			res.status = 200;
			res.set_content(OpenApiDocsHtml(), "text/html; charset=utf-8");
			finish();
			return;
		}
		if (req.path == "/redoc" || req.path == "/redoc/") {
			route_name = "redoc";
			route_path = "/redoc";
			res.status = 200;
			res.set_content(OpenApiRedocHtml(), "text/html; charset=utf-8");
			finish();
			return;
		}
	}

	// Built-in OPTIONS for docs paths: 204 preflight only when CORS is on;
	// otherwise 405 like FastAPI without CORSMiddleware.
	if (req.method == "OPTIONS" && (req.path == "/openapi.json" || req.path == "/docs" || req.path == "/docs/" ||
	                                req.path == "/redoc" || req.path == "/redoc/")) {
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
	RouteMatch match;
	bool path_matched_other_method = false;
	vector<string> methods_for_path;
	auto &qa_state = QuackapiState::Get(*db);
	// Live shared views — no per-request deep copy of the full registry.
	auto routes = qa_state.LiveRoutes();
	for (auto &route : *routes) {
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
	// Auto-HEAD: if HEAD and no explicit HEAD route, reuse the GET handler.
	if (!match.matched && req.method == "HEAD") {
		for (auto &route : *routes) {
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

	if (!match.matched) {
		// Starlette redirect_slashes: if the alternate trailing-slash form would
		// match a registered route, 307 to that path (preserve query).
		// Built-in docs paths already accept both forms above.
		if (!path_matched_other_method && methods_for_path.empty() && req.path != "/") {
			string alt_path;
			if (HasTrailingSlash(req.path)) {
				alt_path = StripTrailingSlash(req.path);
			} else {
				alt_path = req.path + "/";
			}
			bool alt_matches = false;
			for (auto &route : *routes) {
				vector<std::pair<string, string>> captures;
				if (!MatchPattern(route.pattern, alt_path, captures)) {
					continue;
				}
				// Any method registration is enough to redirect (Starlette).
				alt_matches = true;
				break;
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

	// Per-route IO timeout (CREATE ROUTE … TIMEOUT / WITH (timeout_sec := N)).
	// Extends httplib SocketStream select deadlines + SO_RCVTIMEO/SO_SNDTIMEO for
	// this connection through handler execute and response write.
	if (match.route.timeout_sec > 0) {
		ApplyRouteIoTimeout(static_cast<time_t>(match.route.timeout_sec));
	}

	// ---- AUTH ENFORCEMENT (before prepare/execute) ----
	route_name = match.route.name;
	route_path = match.route.pattern;
	// Public routes (require_auth empty) pass through unchanged.
	// Auth is evaluated through the SQL surface (quackapi_verify_auth), the
	// same EvaluateAuthQuery shape quack uses for CONNECTION_REQUEST
	// (duckdb-quack src/quack_server.cpp). CREATE AUTH DDL remains the policy
	// definition layer; quack_authentication_function can point at
	// quackapi_authentication so the RPC plane shares that policy.
	auto headers = CollectHeaders(req);
	auto auth_result = CheckAuth(*db, match.route, headers);
	if (match.route.rate_limit_n > 0 && match.route.rate_limit_per_sec > 0) {
		string by = match.route.rate_limit_by.empty() ? string("ip") : match.route.rate_limit_by;
		int retry_after = match.route.rate_limit_per_sec;
		if (!qa_state.AllowRateLimit(match.route.name, RateLimitClientKey(*db, req, by, match.route, auth_result),
		                             match.route.rate_limit_n, match.route.rate_limit_per_sec, retry_after)) {
			res.set_header("Retry-After", std::to_string(retry_after));
			SetJson(res, 429, "{\"detail\":\"Rate limit exceeded\"}");
			finish();
			return;
		}
	}
	if (!auth_result.ok) {
		if (!auth_result.www_authenticate.empty()) {
			res.set_header("WWW-Authenticate", auth_result.www_authenticate);
		}
		SetJson(res, auth_result.status, auth_result.body);
		finish();
		return;
	}
	// auth_result.claims is ready for $claims_<k> binding below.

	bool authenticated = !match.route.require_auth.empty() && auth_result.ok;
	string handler_sql = match.route.handler_sql;

	try {
		// Destroy the connection before leaving the request. Retaining it in TLS
		// can keep the last DatabaseInstance alive until after DuckDB's allocator
		// TLS has been destroyed, causing a shutdown use-after-free.
		// TEMP/SET and prepared plans also remain strictly request-local.
		Connection con(*db);
		const auto elapsed_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		QuackapiQueryDeadline deadline(con, std::max<int64_t>(1, options.query_timeout_ms - elapsed_ms));
		auto &middleware_registry = QuackapiMiddlewareRegistry::Get(*db);
		if (!middleware_registry.Snapshot(QuackapiMiddlewarePhase::BEFORE, match.route.group_name).empty() ||
		    !middleware_registry.Snapshot(QuackapiMiddlewarePhase::AFTER, match.route.group_name).empty()) {
			auto context = std::make_shared<QuackapiMiddlewareContext>();
			context->method = req.method;
			context->path = req.path;
			context->route_name = match.route.name;
			context->group_name = match.route.group_name;
			context->request_id = request_id;
			context->request_body = req.body;
			context->client_ip = req.remote_addr;
			auto subject = auth_result.claims.find("sub");
			if (subject != auth_result.claims.end()) {
				context->auth_subject = subject->second;
			}
			context->timeout_ms = options.query_timeout_ms;
			context->elapsed_ms = elapsed_ms;
			context->headers_json = PairsToJsonObject(req.headers);
			context->query_json = PairsToJsonObject(req.params);
			if (!ExecuteQuackapiMiddleware(con, *db, QuackapiMiddlewarePhase::BEFORE, context->group_name, *context,
			                               authenticated, auth_result.claims)) {
				SetJson(res, context->status, context->response_body);
				for (const auto &header : context->response_headers) {
					res.set_header(header.first, header.second);
				}
				finish();
				return;
			}
			auto claims = auth_result.claims;
			after_middleware = [context, db, authenticated, claims, &res, t0]() {
				context->status = res.status;
				context->response_body = res.body;
				context->elapsed_ms =
				    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
				        .count();
				Connection middleware_connection(*db);
				if (!ExecuteQuackapiMiddleware(middleware_connection, *db, QuackapiMiddlewarePhase::AFTER,
				                               context->group_name, *context, authenticated, claims)) {
					SetJson(res, context->status, context->response_body);
				}
				for (const auto &header : context->response_headers) {
					res.set_header(header.first, header.second);
				}
			};
		}

		// Request params: path captures shadow query params of the same name.
		// Body fields (JSON / form / multipart) fill remaining names with loc=body.
		case_insensitive_map_t<std::pair<string, string>> provided; // name -> (loc, raw)
		case_insensitive_map_t<bool> explicit_json_nulls;           // body field -> true; missing is absent
		Value validated_typed_body;
		bool has_validated_typed_body = false;
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
		// Server-stamped request id — bindable as $request_id in handler SQL.
		// Wins over any client query/path of the same name.
		provided["request_id"] = {"server", request_id};
		provided["trace_id"] = {"server", trace_id};
		provided["span_id"] = {"server", span_id};

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
					if (provided.find(pspec.name) == provided.end() || provided[pspec.name].first != "path") {
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
					if (provided.find(pspec.name) == provided.end() || provided[pspec.name].first != "path") {
						provided[pspec.name] = {"cookie", cit->second};
					}
				}
			}
		}

		// ---- REQUEST BODY BINDING (POST/PUT/PATCH) ----
		// Body fills `provided` before handler preparation.
		// Named params are inferred from handler SQL.
		bool body_method = IsBodyMethod(req.method);
		bool has_body_named = false;
		bool needs_json_field_extraction = false;
		bool needs_body_fields = !match.route.body_schema.empty() || !match.route.body_type.empty();
		{
			// Use DuckDB's lexer so quoted literals/comments cannot create phantom
			// $body/$field request parameters.
			auto protected_text = ProtectedSqlText(handler_sql);
			for (idx_t i = 0; i < handler_sql.size(); i++) {
				if (protected_text[i] || handler_sql[i] != '$') {
					continue;
				}
				if (i + 1 >= handler_sql.size() ||
				    !((handler_sql[i + 1] >= 'A' && handler_sql[i + 1] <= 'Z') ||
				      (handler_sql[i + 1] >= 'a' && handler_sql[i + 1] <= 'z') || handler_sql[i + 1] == '_')) {
					continue;
				}
				idx_t j = i + 1;
				while (j < handler_sql.size() &&
				       ((handler_sql[j] >= 'A' && handler_sql[j] <= 'Z') ||
				        (handler_sql[j] >= 'a' && handler_sql[j] <= 'z') ||
				        (handler_sql[j] >= '0' && handler_sql[j] <= '9') || handler_sql[j] == '_')) {
					j++;
				}
				string pname = handler_sql.substr(i + 1, j - (i + 1));
				i = j - 1;
				if (StringUtil::Lower(pname) == "body") {
					has_body_named = true;
					needs_body_fields = true;
					continue;
				}
				string ck;
				if (IsClaimsParam(pname, ck)) {
					continue;
				}
				if (provided.find(pname) != provided.end()) {
					continue;
				}
				if (IsPathParam(match.route.pattern, pname)) {
					continue;
				}
				const QuackapiParamSpec *ps = FindParamSpec(match.route.params, pname);
				if (ps && (ps->source == QuackapiParamSource::HEADER || ps->source == QuackapiParamSource::COOKIE)) {
					continue;
				}
				if (ps && ps->has_default) {
					// Defaults make the member optional when absent, but a supplied JSON
					// body must still be extracted and override that default.
					// This flag is consumed only inside the JSON-body branch below, so it
					// must not depend on request media detection at scan time.
					needs_json_field_extraction = true;
					continue;
				}
				needs_body_fields = true;
				needs_json_field_extraction = true;
			}
		}

		if (body_method) {
			if (IsJsonMediaType(media) ||
			    (media.empty() && !req.body.empty() && (req.body[0] == '{' || req.body[0] == '['))) {
				if (req.body.empty() && !needs_body_fields && match.route.body_schema.empty() &&
				    match.route.body_type.empty()) {
					// Empty JSON body with fully-bound query/path params — ignore.
				} else {
					case_insensitive_map_t<JsonBodyField> body_fields;
					string err_json;
					// A native BODY TYPE bound only through $body can be an object or
					// array. It does not need the flat-field extractor; the native
					// transform below validates the complete payload directly.
					if (needs_json_field_extraction) {
						auto parse_result = ParseJsonBodyFields(req.body, body_fields);
						if (parse_result != JsonBodyParseResult::SUCCESS) {
							err_json = parse_result == JsonBodyParseResult::INVALID_JSON
							               ? ValidationErrorJsonBody("JSON decode error", "json_invalid")
							               : ValidationErrorJsonBody(
							                     "Input should be a valid dictionary or object to extract fields from",
							                     "model_attributes_type");
							SetJson(res, 422, err_json);
							finish();
							return;
						}
					}
					if (!match.route.body_schema.empty()) {
						if (!ValidateBodySchema(con, match.route.body_schema, req.body, err_json)) {
							SetJson(res, 422, err_json);
							finish();
							return;
						}
					}
					if (!match.route.body_type.empty()) {
						if (!TransformTypedBody(con, match.route.body_type, req.body, validated_typed_body, err_json)) {
							SetJson(res, 422, err_json);
							finish();
							return;
						}
						has_validated_typed_body = true;
					}
					// Body fields fill missing names only (path/query win).
					for (auto &kv : body_fields) {
						if (provided.find(kv.first) == provided.end()) {
							provided[kv.first] = {"body", kv.second.value};
							if (kv.second.explicit_null) {
								explicit_json_nulls[kv.first] = true;
							}
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
					provided[kv.first] = {"body", kv.second};
				}
				if (has_body_named && provided.find("body") == provided.end()) {
					provided["body"] = {"body", req.body};
				}
			} else if (multipart_ct) {
				for (auto &kv : req.form.fields) {
					provided[kv.first] = {"body", kv.second.content};
				}
				for (auto &kv : req.form.files) {
					provided[kv.first] = {"body", kv.second.content};
					string fname_key = kv.first + "_filename";
					provided[fname_key] = {"body", kv.second.filename};
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
				        ValidationErrorJsonBody("Input should be a valid dictionary or object to extract fields from",
				                                "model_attributes_type"));
				finish();
				return;
			} else if (!match.route.body_schema.empty() || !match.route.body_type.empty()) {
				// BODY SCHEMA / BODY TYPE require JSON.
				SetJson(res, 422,
				        ValidationErrorJsonBody("Input should be a valid dictionary or object to extract fields from",
				                                "model_attributes_type"));
				finish();
				return;
			}
		}

		// DuckDB prepare. Fresh prepare every request:
		// no SQL-keyed body/plan cache — mutable tables and volatile functions
		// (now/uuid) must re-execute.
		if (has_body_named && !match.route.body_type.empty()) {
			string body_sql_type = has_validated_typed_body ? validated_typed_body.type().ToString() : string();
			if (body_sql_type.empty()) {
				string type_error;
				if (!ResolveTypedBodySqlType(con, match.route.body_type, body_sql_type, type_error)) {
					SetInternalError(res, "Invalid BODY TYPE declaration: " + type_error);
					finish();
					return;
				}
			}
			handler_sql = RewriteTypedBodyParameter(handler_sql, body_sql_type);
		}
		auto prepare_started = std::chrono::steady_clock::now();
		auto prepared_owned = con.Prepare(handler_sql);
		sql_prepare_ms =
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepare_started).count();
		if (prepared_owned->HasError()) {
			SetInternalError(res, prepared_owned->GetError());
			finish();
			return;
		}
		PreparedStatement *prepared = prepared_owned.get();
		const BodyFormat body_format = ResolveBodyFormat(match.route, req);

		// The database types the columns: cast each provided param to the type
		// the prepared statement expects. Cast failure -> 422, FastAPI-shaped.
		// $claims_* params are server-verified: bind from claims or SQL NULL
		// (never 422 for a missing claim).
		// PARAM … DEFAULT makes a query/path param optional (bind default/NULL).
		auto expected_types = prepared->GetExpectedParameterTypes();
		case_insensitive_map_t<BoundParameterData> named_values;
		vector<QuackapiValidationIssue> validation_issues;
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
			// A declared PARAM type is the request contract. DuckDB may surface
			// ANY for a cast parameter (for example $optional::INTEGER), so apply
			// the declaration even when the planner did not report UNKNOWN/VARCHAR.
			if (spec && !spec->type_name.empty()) {
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
			} else if (body_method && (IsJsonMediaType(media) || (media.empty() && !req.body.empty() &&
			                                                      (req.body[0] == '{' || req.body[0] == '[')))) {
				// An undeclared handler parameter is body-bound for JSON requests;
				// preserve that source for missing/type errors rather than reporting
				// an invented query location.
				loc_kind = "body";
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
					validation_issues.push_back({ValidationLocJson(loc_kind, param_name), "Field required", "missing"});
					continue;
				}
			} else {
				loc_kind = it->second.first;
				if (explicit_json_nulls.find(param_name) != explicit_json_nulls.end()) {
					// A missing field reaches the default branch above. JSON null reaches
					// here and is only accepted when the BODY SCHEMA admitted null or
					// the parameter explicitly declared DEFAULT NULL.
					bool null_allowed = (loc_kind == "body" && !match.route.body_schema.empty()) ||
					                    (spec && spec->has_default && spec->default_is_null);
					if (!null_allowed) {
						validation_issues.push_back({ValidationLocJson(loc_kind, param_name),
						                             "Input should be a valid " + expected.ToString(), "type_error"});
						continue;
					}
					named_values[param_name] = BoundParameterData(Value());
					continue;
				}
				raw = it->second.second;
			}

			// Strict integral check even when type is UNKNOWN: if the raw value
			// is number-like but not a strict integer, and a cast-to-int would
			// succeed via DuckDB rounding (1.5→2, 1e2→100), reject now.
			// When expected is integral we always require ^-?[0-9]+$.
			// When expected is UNKNOWN, apply the same if raw fails strict int
			// but DefaultTryCastAs to INTEGER succeeds (the FastAPI gap).
			if (IsIntegralType(expected)) {
				if (!IsStrictIntegerString(raw, !IsUnsignedIntegralType(expected))) {
					validation_issues.push_back(
					    {ValidationLocJson(loc_kind, param_name), "Input should be a valid integer", "type_error"});
					continue;
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
							validation_issues.push_back({ValidationLocJson(loc_kind, param_name),
							                             "Input should be a valid integer", "type_error"});
							continue;
						}
					}
				}
			}

			// BODY TYPE reuses the complete native transform already validated for
			// this request before binding $body. Individual $field values still bind
			// from extracted JSON text and use their prepared types.
			if (param_name == "body" && !match.route.body_type.empty() &&
			    (expected.id() == LogicalTypeId::STRUCT || expected.id() == LogicalTypeId::LIST ||
			     expected.id() == LogicalTypeId::MAP)) {
				Value typed_body = validated_typed_body;
				if (!has_validated_typed_body) {
					string err_json;
					if (!TransformTypedBody(con, match.route.body_type, raw, typed_body, err_json)) {
						SetJson(res, 422, err_json);
						finish();
						return;
					}
				}
				if (typed_body.type() != expected) {
					Value casted;
					string cast_error;
					if (!typed_body.DefaultTryCastAs(expected, casted, &cast_error)) {
						SetJson(res, 422,
						        ValidationErrorJson("body", "body", "BODY TYPE does not match handler parameter type",
						                            "type_error"));
						finish();
						return;
					}
					typed_body = casted;
				}
				named_values[param_name] = BoundParameterData(typed_body);
				continue;
			}

			BoundParameterData bound;
			string err_json;
			if (!BindParamValue(raw, expected, loc_kind, param_name, bound, err_json)) {
				validation_issues.push_back({ValidationLocJson(loc_kind, param_name),
				                             "Input should be a valid " + expected.ToString(), "type_error"});
				continue;
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
					validation_issues.push_back({ValidationLocJson(loc_kind, param_name), cmsg, ctype});
					continue;
				}
			}

			named_values[param_name] = bound;
			bound_raw[param_name] = {loc_kind, raw};
			(void)from_default;
		}
		if (!validation_issues.empty()) {
			SetJson(res, 422, QuackapiValidationErrorsJson(validation_issues));
			finish();
			return;
		}

		auto execute_started = std::chrono::steady_clock::now();
		auto result = prepared->Execute(named_values, false);
		sql_execute_ms =
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - execute_started).count();
		if (result->HasError()) {
			// Preserve the status mapping while returning DuckDB's diagnostic so clients can fix bad input.
			// - Conversion errors → 422 with recovered param name (not "_")
			// - LIMIT/OFFSET negative → empty 200 [] (FastAPI unconstrained int)
			// - Other binder/invalid-input from values → 422
			// - True handler bugs → 500 with DuckDB's diagnostic
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
				SetJson(res, 422, ValidationErrorJson(loc_kind, pname, err, "type_error"));
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
				if (pname == "_") {
					// No bound client value is implicated in the error (e.g. the
					// handler raised error() deliberately): the client cannot be
					// blamed, so this is a handler bug → 500, not a 422 with a
					// fabricated param location.
					fprintf(stderr, "quackapi: handler error (no client value implicated) → 500: %s\n", err.c_str());
					SetInternalError(res, err);
				} else {
					SetJson(res, 422, ValidationErrorJson(loc_kind, pname, err, "value_error"));
				}
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

		// HTML/TEXT mode uses the single remaining data column name.
		vector<string> data_names;
		for (auto c : data_cols) {
			data_names.push_back(names[c]);
		}
		auto mode = ResponseModeFor(data_names);
		const bool object_envelope =
		    StringUtil::Lower(match.route.response_envelope.empty() ? "array" : match.route.response_envelope) ==
		    "object";

		idx_t max_response_bytes = static_cast<idx_t>(options.max_response_bytes);
		QuackapiResponseWriter response_writer(max_response_bytes);
		unique_ptr<QuackapiJsonChunkEncoder> json_encoder;
		unique_ptr<QuackapiCsvChunkEncoder> csv_encoder;
		if (!data_cols.empty() && mode == ResponseMode::JSON) {
			if (body_format == BodyFormat::CSV) {
				csv_encoder = make_uniq<QuackapiCsvChunkEncoder>(con, names, result->types, data_cols, response_writer);
			} else {
				JsonRowMode row_mode = JsonRowMode::ARRAY;
				if (body_format == BodyFormat::NDJSON) {
					row_mode = JsonRowMode::NDJSON;
				} else if (object_envelope) {
					row_mode = JsonRowMode::OBJECT;
				}
				json_encoder = make_uniq<QuackapiJsonChunkEncoder>(con, names, result->types, data_cols, row_mode,
				                                                   response_writer);
			}
		}

		string location_value;
		vector<string> set_cookie_values;
		idx_t rows_fetched = 0;
		bool limit_exceeded = false;
		while (true) {
			auto chunk = result->Fetch();
			if (!chunk || chunk->size() == 0) {
				break;
			}
			rows_fetched += chunk->size();
			rows_out = static_cast<int64_t>(rows_fetched);

			// Keep response-control columns outside the body while reading them
			// from every chunk, matching the old first-Location/all-Cookies rules.
			for (idx_t row = 0; row < chunk->size(); row++) {
				if (location_col < chunk->ColumnCount()) {
					auto value = chunk->GetValue(location_col, row);
					if (location_value.empty() && !value.IsNull()) {
						location_value = value.ToString();
					}
				}
				if (set_cookie_col < chunk->ColumnCount()) {
					auto value = chunk->GetValue(set_cookie_col, row);
					if (!value.IsNull()) {
						set_cookie_values.push_back(value.ToString());
					}
				}
			}

			if (mode == ResponseMode::HTML || mode == ResponseMode::TEXT) {
				auto col = data_cols[0];
				auto &vector = chunk->data[col];
				UnifiedVectorFormat format;
				vector.ToUnifiedFormat(chunk->size(), format);
				const bool raw_string_bytes = result->types[col].id() == LogicalTypeId::BLOB ||
				                              result->types[col].id() == LogicalTypeId::VARCHAR ||
				                              result->types[col].IsJSONType();
				const string_t *values = nullptr;
				if (raw_string_bytes) {
					values = format.GetData<string_t>();
				}
				for (idx_t row = 0; row < chunk->size(); row++) {
					auto index = format.sel->get_index(row);
					if (!format.validity.RowIsValid(index)) {
						continue;
					}
					if (raw_string_bytes) {
						auto &value = values[index];
						response_writer.Append(value.GetData(), value.GetSize());
					} else {
						auto value = chunk->GetValue(col, row);
						auto text = value.ToString();
						response_writer.Append(text);
					}
					if (response_writer.ExceededLimit()) {
						limit_exceeded = true;
						break;
					}
				}
			} else if (csv_encoder) {
				csv_encoder->AppendChunk(*chunk);
			} else if (json_encoder) {
				json_encoder->AppendChunk(*chunk);
			}
			if (response_writer.ExceededLimit()) {
				limit_exceeded = true;
				break;
			}
		}

		if (limit_exceeded) {
			SetJson(res, 507, "{\"detail\":\"Response exceeds configured byte limit\"}");
			finish();
			return;
		}

		// 0-row EMPTY STATUS: override success STATUS (any FORMAT / html/text).
		if (rows_fetched == 0 && match.route.empty_status != 0) {
			auto empty_body = EmptyResultBody(match.route);
			if (empty_body.size() > max_response_bytes) {
				SetJson(res, 507, "{\"detail\":\"Response exceeds configured byte limit\"}");
				finish();
				return;
			}
			SetJson(res, match.route.empty_status, empty_body);
			finish();
			return;
		}

		if (!location_value.empty()) {
			res.set_header("Location", location_value);
		}
		for (auto &cookie : set_cookie_values) {
			// httplib Headers is multimap — set_header appends.
			res.set_header("Set-Cookie", cookie);
		}

		// No data columns: empty body (redirect / cookie-only responses).
		if (data_cols.empty()) {
			res.status = match.route.status;
			res.body.clear();
			finish();
			return;
		}

		if (json_encoder) {
			json_encoder->Finish();
		}
		if (csv_encoder) {
			csv_encoder->Finish();
		}
		if (response_writer.ExceededLimit()) {
			SetJson(res, 507, "{\"detail\":\"Response exceeds configured byte limit\"}");
			finish();
			return;
		}

		if (mode == ResponseMode::HTML || mode == ResponseMode::TEXT) {
			res.status = match.route.status;
			res.set_content(response_writer.TakeData(),
			                mode == ResponseMode::HTML ? "text/html; charset=utf-8" : "text/plain; charset=utf-8");
		} else if (body_format == BodyFormat::CSV) {
			res.status = match.route.status;
			res.set_content(response_writer.TakeData(), "text/csv; charset=utf-8");
		} else if (body_format == BodyFormat::NDJSON) {
			res.status = match.route.status;
			res.set_content(response_writer.TakeData(), "application/x-ndjson");
		} else if (object_envelope && rows_fetched > 1) {
			SetJson(res, 500,
			        "{\"detail\":\"ENVELOPE object requires exactly one row, got " + std::to_string(rows_fetched) +
			            "\"}");
		} else {
			res.status = match.route.status;
			res.set_content(response_writer.TakeData(), "application/json");
		}
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
	// In-process (no TCP) servers never set is_running / have no Server object.
	if (is_running.exchange(false) && server) {
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

bool QuackapiPortIsAccepting(const string &host, int port, int connect_timeout_ms) {
	if (port < 1 || port > 65535 || host.empty()) {
		return false;
	}
	if (connect_timeout_ms < 0) {
		connect_timeout_ms = 0;
	}
	const time_t sec = static_cast<time_t>(connect_timeout_ms / 1000);
	const time_t usec = static_cast<time_t>((connect_timeout_ms % 1000) * 1000);
	try {
		duckdb_httplib::Client cli(host, port);
		cli.set_connection_timeout(sec, usec);
		cli.set_read_timeout(1, 0);
		cli.set_write_timeout(1, 0);
		cli.set_tcp_nodelay(true);
		// Any completed HTTP exchange (including 404) means the listener accepts.
		auto res = cli.Get("/");
		auto err = res.error();
		return err == duckdb_httplib::Error::Success || err == duckdb_httplib::Error::Read ||
		       err == duckdb_httplib::Error::Write;
	} catch (...) {
		return false;
	}
}

} // namespace duckdb
