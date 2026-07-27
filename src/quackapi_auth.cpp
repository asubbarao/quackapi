#include "quackapi_auth.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser_extension.hpp"

// Bundled mbedtls — same target httpfs/parquet link as duckdb_mbedtls.
#include "mbedtls/base64.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include <chrono>
#include <cstring>
#include "quackapi_util.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Crypto helpers (JWT HS256 — intentionally self-contained)
//
// Community extension `crypto` (query-farm) exposes crypto_hash / crypto_hmac /
// crypto_random_bytes only. It is NOT a JWT stack: no compact-token parse, no
// alg allowlist, no exp/nbf, no claims map for $claims_*.
//
// JWT verify here uses DuckDB-bundled mbedtls (same as httpfs/parquet) so
// INSTALL quackapi does not require INSTALL crypto, and auth works offline in
// one process. If crypto ever gains first-class JWT verify, re-evaluate; until
// then HMAC-SHA256 + json_* claims parse is the product surface.
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
//===--------------------------------------------------------------------===//
// JWT claims JSON — DuckDB json_* (same substrate as body parse in server.cpp)
//===--------------------------------------------------------------------===//

namespace {

//! Flatten top-level JSON object keys via DuckDB (no hand-rolled recursive parser).
bool ParseJsonObjectClaims(DatabaseInstance &db, const string &json, unordered_map<string, string> &claims,
                           unordered_map<string, bool> &null_claims) {
	Connection con(db);
	auto check = con.Query("SELECT TRY_CAST(? AS JSON) IS NOT NULL", Value(json));
	if (check->HasError()) {
		return false;
	}
	auto check_chunk = check->Fetch();
	if (!check_chunk || check_chunk->size() == 0 || check_chunk->GetValue(0, 0).IsNull() ||
	    !check_chunk->GetValue(0, 0).GetValue<bool>()) {
		return false;
	}
	auto type_res = con.Query("SELECT json_type(?::JSON)", Value(json));
	if (type_res->HasError()) {
		return false;
	}
	auto type_chunk = type_res->Fetch();
	string jtype = type_chunk && type_chunk->size() > 0 && !type_chunk->GetValue(0, 0).IsNull()
	                   ? type_chunk->GetValue(0, 0).GetValue<string>()
	                   : string();
	if (jtype != "OBJECT") {
		return false;
	}
	auto fields_res = con.Query("SELECT k AS key, "
	                            "  CASE json_type(json_extract(doc, '$.' || k)) "
	                            "    WHEN 'VARCHAR' THEN json_extract_string(doc, '$.' || k) "
	                            "    WHEN 'NULL' THEN NULL "
	                            "    ELSE CAST(json_extract(doc, '$.' || k) AS VARCHAR) "
	                            "  END AS val "
	                            "FROM (SELECT ?::JSON AS doc) t, "
	                            "     UNNEST(json_keys(doc)) AS u(k)",
	                            Value(json));
	if (fields_res->HasError()) {
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
				null_claims[key] = true;
			} else {
				claims[key] = val_v.GetValue<string>();
			}
		}
	}
	return true;
}

string DetailJson(const string &detail) {
	// FastAPI-shaped simple string detail (not the 422 array form).
	return "{\"detail\":\"" + QuackapiJsonEscape(detail) + "\"}";
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

//! Fail-closed for a REQUIRE clause that names an auth scheme not present in
//! the registry (missing entirely, or present under a different case). This
//! is client-reachable via any route's REQUIRE — it must never surface as a
//! 500 with the scheme name / internal registry state in the body (fuzz
//! catalog P1 "missing/wrong-case REQUIRE auth scheme"). Every unauthenticated
//! request is rejected exactly as if credentials were absent; the scheme name
//! is deliberately omitted so the response looks identical to an ordinary
//! auth failure rather than advertising server misconfiguration.
QuackapiAuthResult FailClosedUnconfiguredScheme() {
	return Fail401("Not authenticated");
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
//! Full RFC 7519 subset we implement (not "crypto_hmac of the whole token"):
//!   header.payload.signature → base64url decode → alg must be HS256 →
//!   constant-time HMAC-SHA256 over "header.payload" → payload claims via
//!   ParseJsonObjectClaims (DuckDB json_*) → optional exp/nbf checks.
bool VerifyJwtHs256(DatabaseInstance &db, const string &token, const string &secret,
                    unordered_map<string, string> &claims, string &error_detail) {
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
	if (!ParseJsonObjectClaims(db, header_json, header_claims, header_nulls)) {
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
	if (!ParseJsonObjectClaims(db, payload_json, claims, null_claims)) {
		error_detail = "Invalid authentication credentials";
		return false;
	}

	// exp check if present (numeric unix seconds). RFC 7519 §4.1.4: the current
	// time MUST be before exp — reject exp == now, not just exp < now (fuzz
	// catalog P1 "exp == now accepted").
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
			if (exp_val <= NowEpochSeconds()) {
				error_detail = "Invalid authentication credentials";
				return false;
			}
		} catch (...) {
			error_detail = "Invalid authentication credentials";
			return false;
		}
	}

	// nbf check if present (numeric unix seconds). RFC 7519 §4.1.5: the token
	// MUST NOT be accepted before nbf. Previously ignored entirely — a future
	// nbf claim was accepted (fuzz catalog P0-sec "JWT nbf ignored").
	auto nbf_it = claims.find("nbf");
	if (nbf_it != claims.end()) {
		try {
			size_t consumed = 0;
			long long nbf_val = std::stoll(nbf_it->second, &consumed);
			if (consumed != nbf_it->second.size()) {
				error_detail = "Invalid authentication credentials";
				return false;
			}
			if (nbf_val > NowEpochSeconds()) {
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
// Policy engine (CREATE AUTH) — shared by SQL surface + REST
//===--------------------------------------------------------------------===//

string ExtractAuthString(const QuackapiAuth &auth, const case_insensitive_map_t<string> &headers) {
	if (auth.kind == QuackapiAuthKind::API_KEY) {
		string raw_key = HeaderGet(headers, auth.header);
		if (!raw_key.empty()) {
			return raw_key;
		}
		string authorization = HeaderGet(headers, "Authorization");
		if (ExtractBearer(authorization, raw_key)) {
			return raw_key;
		}
		return "";
	}
	if (auth.kind == QuackapiAuthKind::JWT_HS256) {
		string authorization = HeaderGet(headers, "Authorization");
		string token;
		if (ExtractBearer(authorization, token)) {
			return token;
		}
		return "";
	}
	return "";
}

//! Encode claims map as JSON (control-safe QuackapiJsonEscape).
static string ClaimsToJson(const unordered_map<string, string> &claims) {
	string out = "{";
	bool first = true;
	for (auto &kv : claims) {
		if (!first) {
			out += ",";
		}
		first = false;
		out += "\"" + QuackapiJsonEscape(kv.first) + "\":\"" + QuackapiJsonEscape(kv.second) + "\"";
	}
	out += "}";
	return out;
}

//! Parse claims_json produced by ClaimsToJson back into a map (for REST rebind).
static void ParseClaimsJson(DatabaseInstance &db, const string &json, unordered_map<string, string> &claims) {
	unordered_map<string, bool> nulls;
	ParseJsonObjectClaims(db, json, claims, nulls);
}

QuackapiAuthResult VerifyAuthScheme(DatabaseInstance &db, const string &scheme_name, const string &auth_string) {
	QuackapiAuth auth;
	if (!QuackapiState::Get(db).GetAuth(scheme_name, auth)) {
		return FailClosedUnconfiguredScheme();
	}

	if (auth.kind == QuackapiAuthKind::API_KEY) {
		if (auth_string.empty()) {
			return Fail401("Not authenticated", "ApiKey");
		}
		string hash = QuackapiSha256(auth_string);
		auto keys = QuackapiState::Get(db).SnapshotApiKeys(auth.name);
		string subject;
		bool matched = false;
		for (auto &entry : keys) {
			if (QuackapiConstantTimeEquals(hash, entry.key_hash)) {
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
		ok.status = 200;
		ok.claims["sub"] = subject;
		return ok;
	}

	if (auth.kind == QuackapiAuthKind::JWT_HS256) {
		if (auth_string.empty()) {
			return Fail401("Not authenticated", "Bearer");
		}
		unordered_map<string, string> claims;
		string err;
		if (!VerifyJwtHs256(db, auth_string, auth.secret, claims, err)) {
			return Fail401(err);
		}
		QuackapiAuthResult ok;
		ok.ok = true;
		ok.status = 200;
		ok.claims = std::move(claims);
		return ok;
	}

	return Fail500("Unknown authentication kind");
}

//! Mirror of quack_server.cpp EvaluateAuthQuery — run a one-shot SQL auth probe.
template <typename... ARGS>
static Value EvaluateAuthQuery(DatabaseInstance &db, const string &sql, ARGS... values) {
	Connection dummy_connection(db);
	auto auth_result = dummy_connection.Query(sql, values...);
	if (!auth_result || auth_result->HasError()) {
		return Value();
	}
	auto chunk = auth_result->Fetch();
	if (!chunk || chunk->size() == 0) {
		return Value();
	}
	return chunk->GetValue(0, 0);
}

//! Timing-safe token equality — same contract as quack_check_token
//! (duckdb-quack src/quack_extension.cpp QuackAuthToken).
static bool TimingSafeTokenEqual(const string &a, const string &b) {
	return QuackapiConstantTimeEquals(a, b);
}

//===--------------------------------------------------------------------===//
// CheckAuth — REST path goes through SQL (quack EvaluateAuthQuery shape)
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
		return FailClosedUnconfiguredScheme();
	}

	string auth_string = ExtractAuthString(auth, headers);

	// Dispatch via SQL so the REST plane and quack's authentication_function
	// share one policy entrypoint (quackapi_verify_auth). Same pattern as
	// QuackServer::HandleMessageInternal CONNECTION_REQUEST:
	//   SELECT <auth_fn>(?, ?, ?)
	// (duckdb-quack src/quack_server.cpp ~L279-L281).
	auto struct_val =
	    EvaluateAuthQuery(db, "SELECT quackapi_verify_auth(?, ?)", Value(route.require_auth), Value(auth_string));
	if (struct_val.IsNull() || struct_val.type().id() != LogicalTypeId::STRUCT) {
		// Function missing or error — fail closed via direct policy (still no
		// private header-only branch that skips the engine).
		return VerifyAuthScheme(db, route.require_auth, auth_string);
	}

	auto &children = StructValue::GetChildren(struct_val);
	auto &child_types = StructType::GetChildTypes(struct_val.type());
	// Field order: ok, status, body, www_authenticate, claims_json
	if (children.size() < 5) {
		return Fail500("quackapi_verify_auth returned unexpected shape");
	}
	QuackapiAuthResult result;
	result.ok = !children[0].IsNull() && children[0].GetValue<bool>();
	result.status = children[1].IsNull() ? 401 : children[1].GetValue<int32_t>();
	result.body = children[2].IsNull() ? string() : children[2].GetValue<string>();
	result.www_authenticate = children[3].IsNull() ? string() : children[3].GetValue<string>();
	if (result.ok && !children[4].IsNull()) {
		ParseClaimsJson(db, children[4].GetValue<string>(), result.claims);
	}
	(void)child_types;
	return result;
}

//===--------------------------------------------------------------------===//
// quack-compatible auth bridge scalars
//===--------------------------------------------------------------------===//

namespace {

LogicalType VerifyAuthReturnType() {
	child_list_t<LogicalType> children;
	children.emplace_back("ok", LogicalType::BOOLEAN);
	children.emplace_back("status", LogicalType::INTEGER);
	children.emplace_back("body", LogicalType::VARCHAR);
	children.emplace_back("www_authenticate", LogicalType::VARCHAR);
	children.emplace_back("claims_json", LogicalType::VARCHAR);
	return LogicalType::STRUCT(std::move(children));
}

void QuackapiVerifyAuthFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = *state.GetContext().db;
	auto scheme = args.GetValue(0, 0);
	auto auth_string = args.GetValue(1, 0);
	string scheme_s = scheme.IsNull() ? string() : scheme.GetValue<string>();
	string auth_s = auth_string.IsNull() ? string() : auth_string.GetValue<string>();

	auto auth_result = VerifyAuthScheme(db, scheme_s, auth_s);
	vector<Value> fields;
	fields.emplace_back(Value::BOOLEAN(auth_result.ok));
	fields.emplace_back(Value::INTEGER(auth_result.status));
	fields.emplace_back(Value(auth_result.body));
	fields.emplace_back(Value(auth_result.www_authenticate));
	fields.emplace_back(Value(auth_result.ok ? ClaimsToJson(auth_result.claims) : string("{}")));
	result.Reference(Value::STRUCT(VerifyAuthReturnType(), std::move(fields)));
}

//! Drop-in for SET quack_authentication_function = 'quackapi_authentication'.
//! Signature matches quack_check_token (session_id, auth_string, token) → BOOLEAN
//! (duckdb-quack src/quack_extension.cpp L131-L136).
//! Accepts either the server token (timing-safe) OR any registered CREATE AUTH scheme.
void QuackapiAuthenticationFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = *state.GetContext().db;
	// args: session_id, auth_string, token — session_id unused for token equality
	// (same as quack_check_token which only compares args[1] and args[2]).
	string auth_string = args.GetValue(1, 0).IsNull() ? string() : args.GetValue(1, 0).GetValue<string>();
	string token = args.GetValue(2, 0).IsNull() ? string() : args.GetValue(2, 0).GetValue<string>();

	if (!auth_string.empty() && !token.empty() && TimingSafeTokenEqual(auth_string, token)) {
		result.Reference(Value::BOOLEAN(true));
		return;
	}

	// Fall through: try every registered CREATE AUTH scheme against auth_string.
	// Lets a co-located quack_serve share API_KEY/JWT policy with REST.
	auto schemes = QuackapiState::Get(db).SnapshotAuths();
	for (auto &scheme : schemes) {
		auto r = VerifyAuthScheme(db, scheme.name, auth_string);
		if (r.ok) {
			result.Reference(Value::BOOLEAN(true));
			return;
		}
	}
	result.Reference(Value::BOOLEAN(false));
}

//! Drop-in for SET quack_authorization_function = 'quackapi_authorization'.
//! Same contract as quack_nop_authorization: return the query unchanged
//! (duckdb-quack src/quack_extension.cpp QuackDummyAuthorization).
void QuackapiAuthorizationFunction(DataChunk &args, ExpressionState &, Vector &result) {
	// args: session_id, query_string
	result.Reference(args.GetValue(1, 0));
}

} // namespace

void RegisterQuackAuthBridgeFunctions(ExtensionLoader &loader) {
	ScalarFunction verify("quackapi_verify_auth", {LogicalType::VARCHAR, LogicalType::VARCHAR}, VerifyAuthReturnType(),
	                      QuackapiVerifyAuthFunction);
	verify.SetVolatile();
	loader.RegisterFunction(verify);

	ScalarFunction authn("quackapi_authentication", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                     LogicalType::BOOLEAN, QuackapiAuthenticationFunction);
	authn.SetVolatile();
	loader.RegisterFunction(authn);

	ScalarFunction authz("quackapi_authorization", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                     QuackapiAuthorizationFunction);
	authz.SetVolatile();
	loader.RegisterFunction(authz);
}

//===--------------------------------------------------------------------===//
// CREATE AUTH / DROP AUTH parser extension
//===--------------------------------------------------------------------===//

namespace {

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
	auto q = QuackapiTrim(query);
	auto upper = StringUtil::Upper(q);

	bool or_replace = false;
	idx_t pos;
	if (StringUtil::StartsWith(upper, "CREATE AUTH ")) {
		pos = 12;
	} else if (StringUtil::StartsWith(upper, "CREATE OR REPLACE AUTH ")) {
		pos = 23;
		or_replace = true;
	} else if (StringUtil::StartsWith(upper, "DROP AUTH ")) {
		auto name = QuackapiTrim(q.substr(10));
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

	auto rest = QuackapiTrim(q.substr(pos));

	// <name>
	auto first_space = rest.find(' ');
	if (first_space == string::npos) {
		return ParserExtensionParseResult("CREATE AUTH <name> AS API_KEY | JWT (...)");
	}
	auto name = rest.substr(0, first_space);
	rest = QuackapiTrim(rest.substr(first_space));
	auto rest_upper = StringUtil::Upper(rest);

	// AS <kind>
	if (!StringUtil::StartsWith(rest_upper, "AS ")) {
		return ParserExtensionParseResult("Expected AS API_KEY | JWT after auth name");
	}
	rest = QuackapiTrim(rest.substr(3));
	rest_upper = StringUtil::Upper(rest);

	QuackapiAuth auth;
	auth.name = name;

	if (StringUtil::StartsWith(rest_upper, "API_KEY")) {
		auth.kind = QuackapiAuthKind::API_KEY;
		auth.header = "X-API-Key";
		rest = QuackapiTrim(rest.substr(7));
		rest_upper = StringUtil::Upper(rest);
		// optional ( HEADER '<hdr>' )
		if (!rest.empty() && rest[0] == '(') {
			auto close = rest.find(')');
			if (close == string::npos) {
				return ParserExtensionParseResult("Unterminated API_KEY options");
			}
			auto opts = QuackapiTrim(rest.substr(1, close - 1));
			auto opts_upper = StringUtil::Upper(opts);
			if (StringUtil::StartsWith(opts_upper, "HEADER ")) {
				auto hrest = QuackapiTrim(opts.substr(7));
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
			rest = QuackapiTrim(rest.substr(close + 1));
		}
		if (!rest.empty()) {
			return ParserExtensionParseResult("Unexpected tokens after API_KEY");
		}
	} else if (StringUtil::StartsWith(rest_upper, "JWT")) {
		auth.kind = QuackapiAuthKind::JWT_HS256;
		rest = QuackapiTrim(rest.substr(3));
		if (rest.empty() || rest[0] != '(') {
			return ParserExtensionParseResult("JWT requires ( SECRET '<secret>' [, ALGORITHM HS256 ] )");
		}
		auto close = rest.find(')');
		if (close == string::npos) {
			return ParserExtensionParseResult("Unterminated JWT options");
		}
		auto opts = QuackapiTrim(rest.substr(1, close - 1));
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
		rest = QuackapiTrim(rest.substr(close + 1));
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
	string kind; // "API_KEY" / "JWT_HS256"
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
	BindStatusColumn(return_types, names);
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
	EmitOneShotStatus(output, bind_data.finished, message);
}

TableFunction MakeApplyAuthFunction() {
	return MakeApplyDdlFunction("quackapi_apply_auth",
	                            {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                             LogicalType::VARCHAR, LogicalType::VARCHAR},
	                            ApplyAuthExec, ApplyAuthBind);
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
	FinishDdlPlan(result);
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
