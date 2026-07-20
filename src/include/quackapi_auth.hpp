#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser_extension.hpp"

#include "quackapi_state.hpp"

namespace duckdb {

class DatabaseInstance;
class ExtensionLoader;

//===--------------------------------------------------------------------===//
// Crypto helpers (bundled mbedtls — same lib as httpfs / duckdb_mbedtls)
//===--------------------------------------------------------------------===//

//! SHA-256 digest; returns 32 raw bytes.
string QuackapiSha256(const string &data);

//! HMAC-SHA256; returns 32 raw bytes.
string QuackapiHmacSha256(const string &key, const string &data);

//! Constant-time equality for equal-length buffers. Returns false if sizes differ.
bool QuackapiConstantTimeEquals(const string &a, const string &b);

//! Base64url (RFC 7515) encode/decode. Decode returns false on invalid input.
string QuackapiBase64UrlEncode(const string &data);
bool QuackapiBase64UrlDecode(const string &input, string &out);

//===--------------------------------------------------------------------===//
// Auth enforcement (called from HandleRequest BEFORE prepare/execute)
//===--------------------------------------------------------------------===//

struct QuackapiAuthResult {
	bool ok = false;
	int status = 401;
	string body;
	//! Set when a WWW-Authenticate header should be sent (missing credentials).
	string www_authenticate;
	//! Claim name -> string form for binding as $claims_<name>.
	//! Non-string / nested JSON values are stored as compact JSON text.
	//! Absent claims are simply missing from the map (bind SQL NULL).
	unordered_map<string, string> claims;
};

//! Policy engine for one CREATE AUTH scheme against a raw credential string
//! (API key material or JWT). Used by the SQL surface and by REST handlers via
//! EvaluateAuthQuery-style SQL (mirrors quack_server.cpp::EvaluateAuthQuery).
QuackapiAuthResult VerifyAuthScheme(DatabaseInstance &db, const string &scheme_name, const string &auth_string);

//! Extract the credential string a scheme expects from HTTP headers
//! (X-API-Key / custom header / Authorization: Bearer). Empty if missing.
string ExtractAuthString(const QuackapiAuth &auth, const case_insensitive_map_t<string> &headers);

//! Enforce route.require_auth against request headers by dispatching through
//! the registered SQL auth surface (quackapi_verify_auth), not a private
//! in-handler branch — same EvaluateAuthQuery pattern as quack's CONNECTION_REQUEST
//! path (duckdb-quack src/quack_server.cpp).
//! Public routes (empty require_auth) return ok=true with empty claims.
//! Unknown auth name fails closed (ok=false).
//! Never puts secrets or key hashes into body/claims.
QuackapiAuthResult CheckAuth(DatabaseInstance &db, const QuackapiRoute &route,
                             const case_insensitive_map_t<string> &headers);

//===--------------------------------------------------------------------===//
// CREATE AUTH / DROP AUTH parser extension (mirrors CREATE ROUTE pattern)
//===--------------------------------------------------------------------===//

//! CREATE [OR REPLACE] AUTH / DROP AUTH syntax.
class AuthDdlParserExtension : public ParserExtension {
public:
	AuthDdlParserExtension();
};

TableFunction GetApplyAuthFunction();

//===--------------------------------------------------------------------===//
// Table / scalar functions registered from LoadInternal
//===--------------------------------------------------------------------===//

TableFunction GetQuackapiAuthsFunction();
TableFunction GetQuackapiAddApiKeyFunction();

//! Register quack-compatible auth scalars:
//!   quackapi_authentication(session_id, auth_string, token) → BOOLEAN
//!   quackapi_authorization(session_id, query) → VARCHAR
//!   quackapi_verify_auth(scheme, auth_string) → STRUCT(...)
//! so SET quack_authentication_function='quackapi_authentication' works.
void RegisterQuackAuthBridgeFunctions(ExtensionLoader &loader);

} // namespace duckdb
