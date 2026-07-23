#!/usr/bin/env bash
# HTTP integration: REQUIRE auth — API_KEY + JWT → 401 + WWW-Authenticate.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18961}"
INIT="$(mktemp /tmp/quackapi_auth_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE AUTH site AS API_KEY;
SELECT * FROM quackapi_add_api_key('site', 'k-secret', 'alice');
-- Bind $claims_sub so a claims-map regression fails this suite (not hardcoded).
CREATE ROUTE secure GET '/secure' REQUIRE site AS
SELECT true AS ok, $claims_sub AS sub;

CREATE AUTH jwt_auth AS JWT ( SECRET 'conformance-secret' );
CREATE ROUTE jwt_route GET '/jwt' REQUIRE jwt_auth AS
SELECT $claims_sub AS sub, $claims_role AS role;

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. API key happy path (claims_sub bound)"
curl_json GET "/secure" -H "X-API-Key: k-secret"
assert_status "$_QA_LAST_STATUS" "200" "secure_happy"
assert_body_contains "$_QA_LAST_BODY" '"ok":true' "secure_happy ok"
assert_body_contains "$_QA_LAST_BODY" '"sub":"alice"' "secure_happy claims_sub"

echo "-- 2. API key missing → 401 + WWW-Authenticate"
curl_json GET "/secure"
assert_status "$_QA_LAST_STATUS" "401" "secure_missing"
if ! echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -qi '^WWW-Authenticate:'; then
  echo "ASSERT FAIL (secure_missing): WWW-Authenticate missing" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi

echo "-- 3. API key bad → 401"
curl_json GET "/secure" -H "X-API-Key: wrong"
assert_status "$_QA_LAST_STATUS" "401" "secure_bad_key"

echo "-- 4. JWT missing → 401"
curl_json GET "/jwt"
assert_status "$_QA_LAST_STATUS" "401" "jwt_missing"

echo "-- 5. JWT bad token → 401"
curl_json GET "/jwt" -H "Authorization: Bearer not.a.jwt"
assert_status "$_QA_LAST_STATUS" "401" "jwt_bad"

echo "-- 6. JWT happy path (HS256) — claims bound into response"
# Build HS256 JWT with secret conformance-secret, sub=alice, role=admin, exp far future
JWT="$(python3 - <<'PY'
import base64, hashlib, hmac, json, time
def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()
secret = b"conformance-secret"
header = b64url(json.dumps({"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode())
payload = b64url(json.dumps(
    {"sub": "alice", "role": "admin", "exp": int(time.time()) + 3600},
    separators=(",", ":")).encode())
sig = b64url(hmac.new(secret, f"{header}.{payload}".encode(), hashlib.sha256).digest())
print(f"{header}.{payload}.{sig}")
PY
)"
curl_json GET "/jwt" -H "Authorization: Bearer ${JWT}"
assert_status "$_QA_LAST_STATUS" "200" "jwt_happy"
assert_body_contains "$_QA_LAST_BODY" '"sub":"alice"' "jwt_happy claims_sub"
assert_body_contains "$_QA_LAST_BODY" '"role":"admin"' "jwt_happy claims_role"

echo "-- 7. JWT expired → 401"
JWT_EXP="$(python3 - <<'PY'
import base64, hashlib, hmac, json, time
def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()
secret = b"conformance-secret"
header = b64url(json.dumps({"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode())
payload = b64url(json.dumps(
    {"sub": "alice", "exp": int(time.time()) - 10},
    separators=(",", ":")).encode())
sig = b64url(hmac.new(secret, f"{header}.{payload}".encode(), hashlib.sha256).digest())
print(f"{header}.{payload}.{sig}")
PY
)"
curl_json GET "/jwt" -H "Authorization: Bearer ${JWT_EXP}"
assert_status "$_QA_LAST_STATUS" "401" "jwt_expired"

echo "-- 8. JWT alg none → 401"
JWT_NONE="$(python3 - <<'PY'
import base64, json
def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()
header = b64url(json.dumps({"alg": "none", "typ": "JWT"}, separators=(",", ":")).encode())
payload = b64url(json.dumps({"sub": "alice"}, separators=(",", ":")).encode())
print(f"{header}.{payload}.")
PY
)"
curl_json GET "/jwt" -H "Authorization: Bearer ${JWT_NONE}"
assert_status "$_QA_LAST_STATUS" "401" "jwt_alg_none"

echo "auth.test.sh OK"
stop_quackapi
