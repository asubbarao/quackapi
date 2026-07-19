#include "quackapi_auth.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parser_extension.hpp"

// Bundled mbedtls — same target httpfs/parquet link as duckdb_mbedtls.
#include "mbedtls/base64.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include <chrono>
#include <cstring>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Crypto helpers
//===--------------------------------------------------------------------===//

string QuackapiSha256(const string &data) {
	string out;
	out.resize(32);
	mbedtls_sha256_context ctx;
	mbedtls_sha256_init(&ctx);
	if (mbedtls_sha256_starts(&ctx, 0) != 0 ||
	    mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char *>(data.data()), data.size()) != 0 ||
	    mbedtls_sha256_finish(&ctx, reinterpret_cast<unsigned char *>(&out[0])) != 0) {
		mbedtls_sha256_free(&ctx);
		throw InternalException("quackapi: SHA-256 failed");
	}
	mbedtls_sha256_free(&ctx);
	return out;
}

string QuackapiHmacSha256(const string &key, const string &data) {
	string out;
	out.resize(32);
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	if (!md) {
		throw InternalException("quackapi: HMAC-SHA256 md info unavailable");
	}
	mbedtls_md_context_t ctx;
	mbedtls_md_init(&ctx);
	if (mbedtls_md_setup(&ctx, md, 1) != 0 ||
	    mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char *>(key.data()), key.size()) != 0 ||
	    mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char *>(data.data()), data.size()) != 0 ||
	    mbedtls_md_hmac_finish(&ctx, reinterpret_cast<unsigned char *>(&out[0])) != 0) {
		mbedtls_md_free(&ctx);
		throw InternalException("quackapi: HMAC-SHA256 failed");
	}
	mbedtls_md_free(&ctx);
	return out;
}

bool QuackapiConstantTimeEquals(const string &a, const string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	if (a.empty()) {
		return true;
	}
	return mbedtls_ct_memcmp(a.data(), b.data(), a.size()) == 0;
}

string QuackapiBase64UrlEncode(const string &data) {
	size_t olen = 0;
	mbedtls_base64_encode(nullptr, 0, &olen, reinterpret_cast<const unsigned char *>(data.data()), data.size());
	string std_b64;
	std_b64.resize(olen);
	if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(&std_b64[0]), std_b64.size(), &olen,
	                          reinterpret_cast<const unsigned char *>(data.data()), data.size()) != 0) {
		throw InternalException("quackapi: base64 encode failed");
	}
	std_b64.resize(olen);
	// Standard base64 → base64url: +/ → -_, strip padding.
	string out;
	out.reserve(std_b64.size());
	for (char c : std_b64) {
		if (c == '+') {
			out += '-';
		} else if (c == '/') {
			out += '_';
		} else if (c == '=') {
			// drop padding
		} else {
			out += c;
		}
	}
	return out;
}

bool QuackapiBase64UrlDecode(const string &input, string &out) {
	// base64url → standard base64
	string std_b64;
	std_b64.reserve(input.size() + 4);
	for (char c : input) {
		if (c == '-') {
			std_b64 += '+';
		} else if (c == '_') {
			std_b64 += '/';
		} else {
			std_b64 += c;
		}
	}
	while (std_b64.size() % 4 != 0) {
		std_b64 += '=';
	}
	size_t olen = 0;
	int rc = mbedtls_base64_decode(nullptr, 0, &olen, reinterpret_cast<const unsigned char *>(std_b64.data()),
	                               std_b64.size());
	// First call returns BUFFER_TOO_SMALL with the needed size when dst is null —
	// or 0 with olen for empty. Accept either path that yields a size.
	if (rc != 0 && rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
		return false;
	}
	out.resize(olen);
	if (olen == 0) {
		return true;
	}
	if (mbedtls_base64_decode(reinterpret_cast<unsigned char *>(&out[0]), out.size(), &olen,
	                          reinterpret_cast<const unsigned char *>(std_b64.data()), std_b64.size()) != 0) {
		out.clear();
		return false;
	}
	out.resize(olen);
	return true;
}

//===--------------------------------------------------------------------===//
// Minimal top-level JSON object claim extraction for JWT payloads
//===--------------------------------------------------------------------===//

namespace {

void SkipWs(const string &s, idx_t &i) {
	while (i < s.size() && StringUtil::CharacterIsSpace(s[i])) {
		i++;
	}
}

bool ParseJsonString(const string &s, idx_t &i, string &out) {
	if (i >= s.size() || s[i] != '"') {
		return false;
	}
	i++;
	out.clear();
	while (i < s.size()) {
		char c = s[i++];
		if (c == '"') {
			return true;
		}
		if (c == '\\' && i < s.size()) {
			char e = s[i++];
			switch (e) {
			case '"':
			case '\\':
			case '/':
				out += e;
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'n':
				out += '\n';
				break;
			case 'r':
				out += '\r';
				break;
			case 't':
				out += '\t';
				break;
			case 'u': {
				// Keep \uXXXX as literal UTF-16 code unit hex for VARCHAR binding.
				if (i + 4 > s.size()) {
					return false;
				}
				out += "\\u";
				out += s.substr(i, 4);
				i += 4;
				break;
			}
			default:
				out += e;
				break;
			}
		} else {
			out += c;
		}
	}
	return false;
}

//! Parse a JSON value starting at i; advance i past it. For objects/arrays the
//! raw JSON substring is returned (for nested claims). Strings return unquoted
//! content. Numbers/bools/null return their literal text ("null", "true", "42").
bool ParseJsonValue(const string &s, idx_t &i, string &out, bool &is_null) {
	SkipWs(s, i);
	if (i >= s.size()) {
		return false;
	}
	is_null = false;
	char c = s[i];
	if (c == '"') {
		return ParseJsonString(s, i, out);
	}
	if (c == '{' || c == '[') {
		// Capture balanced raw JSON.
		char open = c;
		char close = (c == '{') ? '}' : ']';
		idx_t start = i;
		int depth = 0;
		bool in_str = false;
		bool esc = false;
		for (; i < s.size(); i++) {
			char ch = s[i];
			if (in_str) {
				if (esc) {
					esc = false;
				} else if (ch == '\\') {
					esc = true;
				} else if (ch == '"') {
					in_str = false;
				}
				continue;
			}
			if (ch == '"') {
				in_str = true;
				continue;
			}
			if (ch == open) {
				depth++;
			} else if (ch == close) {
				depth--;
				if (depth == 0) {
					i++;
					out = s.substr(start, i - start);
					return true;
				}
			}
		}
		return false;
	}
	// literal: null / true / false / number
	idx_t start = i;
	if (s.compare(i, 4, "null") == 0) {
		i += 4;
		out = "null";
		is_null = true;
		return true;
	}
	if (s.compare(i, 4, "true") == 0) {
		i += 4;
		out = "true";
		return true;
	}
	if (s.compare(i, 5, "false") == 0) {
		i += 5;
		out = "false";
		return true;
	}
	// number
	if (c == '-' || (c >= '0' && c <= '9')) {
		if (s[i] == '-') {
			i++;
		}
		while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
			i++;
		}
		if (i < s.size() && s[i] == '.') {
			i++;
			while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
				i++;
			}
		}
		if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
			i++;
			if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
				i++;
			}
			while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
				i++;
			}
		}
		out = s.substr(start, i - start);
		return !out.empty() && out != "-";
	}
	return false;
}

bool ParseJsonObjectClaims(const string &json, unordered_map<string, string> &claims,
                           unordered_map<string, bool> &null_claims) {
	idx_t i = 0;
	SkipWs(json, i);
	if (i >= json.size() || json[i] != '{') {
		return false;
	}
	i++;
	SkipWs(json, i);
	if (i < json.size() && json[i] == '}') {
		return true;
	}
	while (i < json.size()) {
		SkipWs(json, i);
		string key;
		if (!ParseJsonString(json, i, key)) {
			return false;
		}
		SkipWs(json, i);
		if (i >= json.size() || json[i] != ':') {
			return false;
		}
		i++;
		string val;
		bool is_null = false;
		if (!ParseJsonValue(json, i, val, is_null)) {
			return false;
		}
		if (is_null) {
			null_claims[key] = true;
		} else {
			claims[key] = val;
		}
		SkipWs(json, i);
		if (i < json.size() && json[i] == ',') {
			i++;
			continue;
		}
		if (i < json.size() && json[i] == '}') {
			return true;
		}
		return false;
	}
	return false;
}

string DetailJson(const string &detail) {
	// FastAPI-shaped simple string detail (not the 422 array form).
	string esc;
	for (unsigned char c : detail) {
		if (c == '"') {
			esc += "\\\"";
		} else if (c == '\\') {
			esc += "\\\\";
		} else if (c < 0x20) {
			char buf[8];
			snprintf(buf, sizeof(buf), "\\u%04x", c);
			esc += buf;
		} else {
			esc += static_cast<char>(c);
		}
	}
	return "{\"detail\":\"" + esc + "\"}";
}

QuackapiAuthResult Fail401(const string &detail, const string &www_auth = "") {
	QuackapiAuthResult r;
	r.ok = false;
	r.status = 401;
	r.body = DetailJson(detail);
	r.www_authenticate = www_auth;
	return r;
}

QuackapiAuthResult Fail500(const string &detail) {
	QuackapiAuthResult r;
	r.ok = false;
	r.status = 500;
	r.body = DetailJson(detail);
	return r;
}

bool ExtractBearer(const string &authorization, string &token) {
	// "Bearer <token>" — case-insensitive scheme.
	if (authorization.size() < 7) {
		return false;
	}
	auto scheme = StringUtil::Lower(authorization.substr(0, 6));
	if (scheme != "bearer") {
		return false;
	}
	idx_t i = 6;
	while (i < authorization.size() && StringUtil::CharacterIsSpace(authorization[i])) {
		i++;
	}
	if (i >= authorization.size()) {
		return false;
	}
	token = authorization.substr(i);
	return !token.empty();
}

int64_t NowEpochSeconds() {
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

//! Verify HS256 JWT. On success fills claims map.
bool VerifyJwtHs256(const string &token, const string &secret, unordered_map<string, string> &claims,
                    string &error_detail) {
	// header.payload.signature
	auto d1 = token.find('.');
	if (d1 == string::npos) {
		error_detail = "Invalid authentication credentials";
		return false;
	}
	auto d2 = token.find('.', d1 + 1);
	if (d2 == string::npos) {
		error_detail = "Invalid authentication credentials";
		return false;
	}
	if (token.find('.', d2 + 1) != string::npos) {
		error_detail = "Invalid authentication credentials";
		return false;
	}
	string header_b64 = token.substr(0, d1);
	string payload_b64 = token.substr(d1 + 1, d2 - d1 - 1);
	string sig_b64 = token.substr(d2 + 1);
	if (header_b64.empty() || payload_b64.empty() || sig_b64.empty()) {
		error_detail = "Invalid authentication credentials";
		return false;
	}

	string signing_input = token.substr(0, d2); // header.payload
	string expected_sig = QuackapiHmacSha256(secret, signing_input);
	string actual_sig;
	if (!QuackapiBase64UrlDecode(sig_b64, actual_sig)) {
		error_detail = "Invalid authentication credentials";
		return false;
	}
	if (!QuackapiConstantTimeEquals(expected_sig, actual_sig)) {
		error_detail = "Invalid authentication credentials";
		return false;
	}

	string header_json;
	if (!QuackapiBase64UrlDecode(header_b64, header_json)) {
		error_detail = "Invalid authentication credentials";
		return false;
	}
	unordered_map<string, string> header_claims;
	unordered_map<string, bool> header_nulls;
	if (!ParseJsonObjectClaims(header_json, header_claims, header_nulls)) {
		error_detail = "Invalid authentication credentials";
		return false;
	}
	auto alg_it = header_claims.find("alg");
	if (alg_it == header_claims.end() || alg_it->second != "HS256") {
		// Reject "none" and any non-HS256 algorithm.
		error_detail = "Invalid authentication credentials";
		return false;
	}

	string payload_json;
	if (!QuackapiBase64UrlDecode(payload_b64, payload_json)) {
		error_detail = "Invalid authentication credentials";
		return false;
	}
	unordered_map<string, bool> null_claims;
	if (!ParseJsonObjectClaims(payload_json, claims, null_claims)) {
		error_detail = "Invalid authentication credentials";
		return false;
	}

	// exp check if present (numeric unix seconds)
	auto exp_it = claims.find("exp");
	if (exp_it != claims.end()) {
		// payload numbers are stored as literal text
		try {
			// allow integer seconds only
			size_t consumed = 0;
			long long exp_val = std::stoll(exp_it->second, &consumed);
			if (consumed != exp_it->second.size()) {
				error_detail = "Invalid authentication credentials";
				return false;
			}
			if (exp_val < NowEpochSeconds()) {
				error_detail = "Invalid authentication credentials";
				return false;
			}
		} catch (...) {
			error_detail = "Invalid authentication credentials";
			return false;
		}
	}
	return true;
}

string HeaderGet(const case_insensitive_map_t<string> &headers, const string &name) {
	auto it = headers.find(name);
	if (it == headers.end()) {
		return "";
	}
	return it->second;
}

} // namespace

//===--------------------------------------------------------------------===//
// CheckAuth
//===--------------------------------------------------------------------===//

QuackapiAuthResult CheckAuth(DatabaseInstance &db, const QuackapiRoute &route,
                             const case_insensitive_map_t<string> &headers) {
	if (route.require_auth.empty()) {
		QuackapiAuthResult ok;
		ok.ok = true;
		return ok;
	}

	QuackapiAuth auth;
	if (!QuackapiState::Get(db).GetAuth(route.require_auth, auth)) {
		// Fail closed: required scheme is not configured.
		return Fail500("Authentication scheme \"" + route.require_auth + "\" is not configured");
	}

	if (auth.kind == QuackapiAuthKind::API_KEY) {
		string raw_key;
		// 1) configured header
		string hdr = HeaderGet(headers, auth.header);
		if (!hdr.empty()) {
			raw_key = hdr;
		} else {
			// 2) Authorization: Bearer <key>
			string authorization = HeaderGet(headers, "Authorization");
			if (!ExtractBearer(authorization, raw_key)) {
				return Fail401("Not authenticated", "ApiKey");
			}
		}
		if (raw_key.empty()) {
			return Fail401("Not authenticated", "ApiKey");
		}

		string hash = QuackapiSha256(raw_key);
		auto keys = QuackapiState::Get(db).SnapshotApiKeys(auth.name);
		string subject;
		bool matched = false;
		// Compare against every stored hash (constant-time per compare).
		for (auto &entry : keys) {
			if (QuackapiConstantTimeEquals(hash, entry.key_hash)) {
				// Prefer first match for subject; still scan all for timing smoothness.
				if (!matched) {
					subject = entry.subject;
					matched = true;
				}
			}
		}
		if (!matched) {
			return Fail401("Invalid authentication credentials");
		}
		QuackapiAuthResult ok;
		ok.ok = true;
		ok.claims["sub"] = subject;
		return ok;
	}

	if (auth.kind == QuackapiAuthKind::JWT_HS256) {
		string authorization = HeaderGet(headers, "Authorization");
		string token;
		if (!ExtractBearer(authorization, token)) {
			return Fail401("Not authenticated", "Bearer");
		}
		unordered_map<string, string> claims;
		string err;
		if (!VerifyJwtHs256(token, auth.secret, claims, err)) {
			return Fail401(err);
		}
		QuackapiAuthResult ok;
		ok.ok = true;
		ok.claims = std::move(claims);
		return ok;
	}

	return Fail500("Unknown authentication kind");
}

//===--------------------------------------------------------------------===//
// CREATE AUTH / DROP AUTH parser extension
//===--------------------------------------------------------------------===//

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

//! Parsed CREATE/DROP AUTH, carried from parse to plan.
struct AuthDdlParseData : public ParserExtensionParseData {
	string action; // CREATE / DROP
	bool or_replace = false;
	QuackapiAuth auth;

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto copy = make_uniq<AuthDdlParseData>();
		copy->action = action;
		copy->or_replace = or_replace;
		copy->auth = auth;
		return std::move(copy);
	}
	string ToString() const override {
		return action + " AUTH " + auth.name;
	}
};

//! Grammar:
//!   CREATE [OR REPLACE] AUTH <name> AS API_KEY [ ( HEADER '<hdr>' ) ];
//!   CREATE [OR REPLACE] AUTH <name> AS JWT ( SECRET '<secret>' [, ALGORITHM HS256 ] );
//!   DROP AUTH <name>;
ParserExtensionParseResult AuthDdlParse(ParserExtensionInfo *, const string &query) {
	auto q = Trim(query);
	auto upper = StringUtil::Upper(q);

	bool or_replace = false;
	idx_t pos;
	if (StringUtil::StartsWith(upper, "CREATE AUTH ")) {
		pos = 12;
	} else if (StringUtil::StartsWith(upper, "CREATE OR REPLACE AUTH ")) {
		pos = 23;
		or_replace = true;
	} else if (StringUtil::StartsWith(upper, "DROP AUTH ")) {
		auto name = Trim(q.substr(10));
		if (name.empty() || name.find(' ') != string::npos) {
			return ParserExtensionParseResult("DROP AUTH expects a single auth name");
		}
		auto data = make_uniq<AuthDdlParseData>();
		data->action = "DROP";
		data->auth.name = name;
		return ParserExtensionParseResult(std::move(data));
	} else {
		return ParserExtensionParseResult();
	}

	auto rest = Trim(q.substr(pos));

	// <name>
	auto first_space = rest.find(' ');
	if (first_space == string::npos) {
		return ParserExtensionParseResult("CREATE AUTH <name> AS API_KEY | JWT (...)");
	}
	auto name = rest.substr(0, first_space);
	rest = Trim(rest.substr(first_space));
	auto rest_upper = StringUtil::Upper(rest);

	// AS <kind>
	if (!StringUtil::StartsWith(rest_upper, "AS ")) {
		return ParserExtensionParseResult("Expected AS API_KEY | JWT after auth name");
	}
	rest = Trim(rest.substr(3));
	rest_upper = StringUtil::Upper(rest);

	QuackapiAuth auth;
	auth.name = name;

	if (StringUtil::StartsWith(rest_upper, "API_KEY")) {
		auth.kind = QuackapiAuthKind::API_KEY;
		auth.header = "X-API-Key";
		rest = Trim(rest.substr(7));
		rest_upper = StringUtil::Upper(rest);
		// optional ( HEADER '<hdr>' )
		if (!rest.empty() && rest[0] == '(') {
			auto close = rest.find(')');
			if (close == string::npos) {
				return ParserExtensionParseResult("Unterminated API_KEY options");
			}
			auto opts = Trim(rest.substr(1, close - 1));
			auto opts_upper = StringUtil::Upper(opts);
			if (StringUtil::StartsWith(opts_upper, "HEADER ")) {
				auto hrest = Trim(opts.substr(7));
				if (hrest.size() < 2 || hrest.front() != '\'' || hrest.back() != '\'') {
					return ParserExtensionParseResult("HEADER expects a quoted string");
				}
				auth.header = hrest.substr(1, hrest.size() - 2);
				if (auth.header.empty()) {
					return ParserExtensionParseResult("HEADER must not be empty");
				}
			} else if (!opts.empty()) {
				return ParserExtensionParseResult("API_KEY options: expected HEADER '<name>'");
			}
			rest = Trim(rest.substr(close + 1));
		}
		if (!rest.empty()) {
			return ParserExtensionParseResult("Unexpected tokens after API_KEY");
		}
	} else if (StringUtil::StartsWith(rest_upper, "JWT")) {
		auth.kind = QuackapiAuthKind::JWT_HS256;
		rest = Trim(rest.substr(3));
		if (rest.empty() || rest[0] != '(') {
			return ParserExtensionParseResult("JWT requires ( SECRET '<secret>' [, ALGORITHM HS256 ] )");
		}
		auto close = rest.find(')');
		if (close == string::npos) {
			return ParserExtensionParseResult("Unterminated JWT options");
		}
		auto opts = Trim(rest.substr(1, close - 1));
		if (opts.empty()) {
			return ParserExtensionParseResult("JWT requires SECRET '<secret>'");
		}
		// Parse comma-separated SECRET '...' and optional ALGORITHM HS256
		bool have_secret = false;
		idx_t oi = 0;
		while (oi < opts.size()) {
			while (oi < opts.size() && (StringUtil::CharacterIsSpace(opts[oi]) || opts[oi] == ',')) {
				oi++;
			}
			if (oi >= opts.size()) {
				break;
			}
			auto slice = opts.substr(oi);
			auto slice_upper = StringUtil::Upper(slice);
			if (StringUtil::StartsWith(slice_upper, "SECRET")) {
				idx_t p = oi + 6; // past "SECRET"
				while (p < opts.size() && StringUtil::CharacterIsSpace(opts[p])) {
					p++;
				}
				if (p >= opts.size() || opts[p] != '\'') {
					return ParserExtensionParseResult("SECRET expects a quoted string");
				}
				auto qend = opts.find('\'', p + 1);
				if (qend == string::npos) {
					return ParserExtensionParseResult("Unterminated SECRET string");
				}
				auth.secret = opts.substr(p + 1, qend - p - 1);
				if (auth.secret.empty()) {
					return ParserExtensionParseResult("SECRET must not be empty");
				}
				have_secret = true;
				oi = qend + 1;
			} else if (StringUtil::StartsWith(slice_upper, "ALGORITHM")) {
				idx_t p = oi + 9; // past "ALGORITHM"
				while (p < opts.size() && StringUtil::CharacterIsSpace(opts[p])) {
					p++;
				}
				idx_t end = p;
				while (end < opts.size() && !StringUtil::CharacterIsSpace(opts[end]) && opts[end] != ',') {
					end++;
				}
				auto alg = StringUtil::Upper(opts.substr(p, end - p));
				if (alg != "HS256") {
					return ParserExtensionParseResult("Only ALGORITHM HS256 is supported");
				}
				oi = end;
			} else {
				return ParserExtensionParseResult("JWT options: SECRET '<secret>' [, ALGORITHM HS256 ]");
			}
		}
		if (!have_secret) {
			return ParserExtensionParseResult("JWT requires SECRET '<secret>'");
		}
		rest = Trim(rest.substr(close + 1));
		if (!rest.empty()) {
			return ParserExtensionParseResult("Unexpected tokens after JWT options");
		}
	} else {
		return ParserExtensionParseResult("Unknown AUTH kind — expected API_KEY or JWT");
	}

	auto data = make_uniq<AuthDdlParseData>();
	data->action = "CREATE";
	data->or_replace = or_replace;
	data->auth = auth;
	return ParserExtensionParseResult(std::move(data));
}

struct ApplyAuthBindData : public TableFunctionData {
	string action;
	bool or_replace = false;
	string name;
	string kind;   // "API_KEY" / "JWT_HS256"
	string header;
	string secret;
	bool finished = false;
};

unique_ptr<FunctionData> ApplyAuthBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ApplyAuthBindData>();
	bind_data->action = input.inputs[0].GetValue<string>();
	bind_data->or_replace = input.inputs[1].GetValue<bool>();
	bind_data->name = input.inputs[2].GetValue<string>();
	bind_data->kind = input.inputs[3].GetValue<string>();
	bind_data->header = input.inputs[4].GetValue<string>();
	bind_data->secret = input.inputs[5].GetValue<string>();
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");
	return std::move(bind_data);
}

void ApplyAuthExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ApplyAuthBindData>();
	if (bind_data.finished) {
		return;
	}
	auto &state = QuackapiState::Get(*context.db);
	string message;
	if (bind_data.action == "CREATE") {
		QuackapiAuth auth;
		auth.name = bind_data.name;
		if (bind_data.kind == "API_KEY") {
			auth.kind = QuackapiAuthKind::API_KEY;
			auth.header = bind_data.header.empty() ? "X-API-Key" : bind_data.header;
		} else if (bind_data.kind == "JWT_HS256") {
			auth.kind = QuackapiAuthKind::JWT_HS256;
			auth.secret = bind_data.secret;
		} else {
			throw InvalidInputException("Unknown auth kind \"%s\"", bind_data.kind);
		}
		state.AddAuth(auth, bind_data.or_replace);
		message = StringUtil::Format("Auth %s: %s", auth.name, bind_data.kind);
	} else {
		if (state.DropAuth(bind_data.name)) {
			message = StringUtil::Format("Dropped auth %s", bind_data.name);
		} else {
			throw InvalidInputException("Auth \"%s\" does not exist", bind_data.name);
		}
	}
	output.SetValue(0, 0, Value(message));
	output.SetCardinality(1);
	bind_data.finished = true;
}

TableFunction MakeApplyAuthFunction() {
	TableFunction function("quackapi_apply_auth",
	                       {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                        LogicalType::VARCHAR, LogicalType::VARCHAR},
	                       ApplyAuthExec, ApplyAuthBind);
	return function;
}

string AuthKindToString(QuackapiAuthKind kind) {
	switch (kind) {
	case QuackapiAuthKind::API_KEY:
		return "API_KEY";
	case QuackapiAuthKind::JWT_HS256:
		return "JWT_HS256";
	}
	return "UNKNOWN";
}

ParserExtensionPlanResult AuthDdlPlan(ParserExtensionInfo *, ClientContext &,
                                      unique_ptr<ParserExtensionParseData> parse_data) {
	auto &data = static_cast<AuthDdlParseData &>(*parse_data);
	ParserExtensionPlanResult result;
	result.function = MakeApplyAuthFunction();
	result.parameters.push_back(Value(data.action));
	result.parameters.push_back(Value::BOOLEAN(data.or_replace));
	result.parameters.push_back(Value(data.auth.name));
	result.parameters.push_back(Value(AuthKindToString(data.auth.kind)));
	result.parameters.push_back(Value(data.auth.header));
	// Secret is passed only through the plan→exec pipeline; never exposed via
	// quackapi_auths() or HTTP responses.
	result.parameters.push_back(Value(data.auth.secret));
	result.requires_valid_transaction = false;
	result.return_type = StatementReturnType::QUERY_RESULT;
	return result;
}

//===--------------------------------------------------------------------===//
// quackapi_auths() — name, kind, header (NEVER secret)
//===--------------------------------------------------------------------===//

struct AuthsBindData : public TableFunctionData {
	bool finished = false;
};

unique_ptr<FunctionData> AuthsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                   vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("kind");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("header");
	return make_uniq<AuthsBindData>();
}

void AuthsExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<AuthsBindData>();
	if (bind_data.finished) {
		return;
	}
	auto auths = QuackapiState::Get(*context.db).SnapshotAuths();
	idx_t row = 0;
	for (auto &auth : auths) {
		output.SetValue(0, row, Value(auth.name));
		output.SetValue(1, row, Value(AuthKindToString(auth.kind)));
		// For JWT, header is empty/unused; report Authorization conceptually.
		if (auth.kind == QuackapiAuthKind::JWT_HS256) {
			output.SetValue(2, row, Value("Authorization"));
		} else {
			output.SetValue(2, row, Value(auth.header));
		}
		row++;
	}
	output.SetCardinality(row);
	bind_data.finished = true;
}

//===--------------------------------------------------------------------===//
// quackapi_add_api_key(auth_name, raw_key, subject) → subject
//===--------------------------------------------------------------------===//

struct AddApiKeyBindData : public TableFunctionData {
	string auth_name;
	string raw_key;
	string subject;
	bool finished = false;
};

unique_ptr<FunctionData> AddApiKeyBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<AddApiKeyBindData>();
	bind_data->auth_name = input.inputs[0].GetValue<string>();
	bind_data->raw_key = input.inputs[1].GetValue<string>();
	bind_data->subject = input.inputs[2].GetValue<string>();
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("subject");
	return std::move(bind_data);
}

void AddApiKeyExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<AddApiKeyBindData>();
	if (bind_data.finished) {
		return;
	}
	auto subject = QuackapiState::Get(*context.db).AddApiKey(bind_data.auth_name, bind_data.raw_key, bind_data.subject);
	output.SetValue(0, 0, Value(subject));
	output.SetCardinality(1);
	bind_data.finished = true;
}

} // namespace

AuthDdlParserExtension::AuthDdlParserExtension() {
	parse_function = AuthDdlParse;
	plan_function = AuthDdlPlan;
}

TableFunction GetApplyAuthFunction() {
	return MakeApplyAuthFunction();
}

TableFunction GetQuackapiAuthsFunction() {
	return TableFunction("quackapi_auths", {}, AuthsExec, AuthsBind);
}

TableFunction GetQuackapiAddApiKeyFunction() {
	return TableFunction("quackapi_add_api_key", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                     AddApiKeyExec, AddApiKeyBind);
}

} // namespace duckdb
