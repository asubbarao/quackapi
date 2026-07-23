#!/usr/bin/env bash
# HTTP: two JWTs hit the same policied route — different rows + masked columns.
# Unauthenticated → 403 (policy fail-closed).
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18973}"
INIT="$(mktemp /tmp/quackapi_policy_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE TABLE pol_orders (id INTEGER, tenant_id VARCHAR, amount INTEGER);
INSERT INTO pol_orders VALUES
  (1, 'acme', 100),
  (2, 'beta', 200),
  (3, 'acme', 150);

CREATE TABLE pol_users (id INTEGER, email VARCHAR, tenant_id VARCHAR);
INSERT INTO pol_users VALUES
  (1, 'alice@acme.test', 'acme'),
  (2, 'bob@beta.test', 'beta');

CREATE AUTH jwt_pol AS JWT ( SECRET 'policy-http-secret' );

CREATE ROW ACCESS POLICY tenant_isolation
  AS (tenant_id VARCHAR) RETURNS BOOLEAN
  USING (tenant_id = $claims_tenant OR $claims_role = 'admin');

CREATE MASKING POLICY mask_email ON VARCHAR
  USING (CASE WHEN $claims_role = 'admin' THEN val ELSE '***' END);

ALTER TABLE pol_orders ADD ROW ACCESS POLICY tenant_isolation ON (tenant_id);
ALTER TABLE pol_users ADD ROW ACCESS POLICY tenant_isolation ON (tenant_id);
ALTER TABLE pol_users MODIFY COLUMN email SET MASKING POLICY mask_email;

CREATE ROUTE list_orders GET '/orders' REQUIRE jwt_pol AS
  SELECT id, tenant_id, amount FROM pol_orders ORDER BY id;

CREATE ROUTE list_users GET '/users' REQUIRE jwt_pol AS
  SELECT id, email, tenant_id FROM pol_users ORDER BY id;

CREATE ROUTE public_orders GET '/public_orders' AS
  SELECT id, tenant_id FROM pol_orders;

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

make_jwt() {
  local tenant="$1" role="$2"
  python3 - <<PY
import base64, hashlib, hmac, json, time
def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()
secret = b"policy-http-secret"
header = b64url(json.dumps({"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode())
payload = b64url(json.dumps({
    "sub": "${tenant}-user",
    "tenant": "${tenant}",
    "role": "${role}",
    "exp": int(time.time()) + 3600
}, separators=(",", ":")).encode())
sig = b64url(hmac.new(secret, f"{header}.{payload}".encode(), hashlib.sha256).digest())
print(f"{header}.{payload}.{sig}")
PY
}

JWT_ACME="$(make_jwt acme user)"
JWT_BETA="$(make_jwt beta user)"
JWT_ADMIN="$(make_jwt acme admin)"

echo "-- 1. Unauthenticated public route on policied table → 403"
curl_json GET "/public_orders"
assert_status "$_QA_LAST_STATUS" "403" "unauth_public_orders"
assert_body_contains "$_QA_LAST_BODY" "Policy denies" "unauth_public_orders body"

echo "-- 2. Missing JWT on REQUIRE route → 401 (auth before policy)"
curl_json GET "/orders"
assert_status "$_QA_LAST_STATUS" "401" "orders_missing_jwt"

echo "-- 3. acme user sees only acme rows"
curl_json GET "/orders" -H "Authorization: Bearer ${JWT_ACME}"
assert_status "$_QA_LAST_STATUS" "200" "orders_acme"
assert_body_contains "$_QA_LAST_BODY" '"tenant_id":"acme"' "orders_acme has acme"
assert_body_not_contains "$_QA_LAST_BODY" '"tenant_id":"beta"' "orders_acme no beta"
assert_body_contains "$_QA_LAST_BODY" '"amount":100' "orders_acme 100"
assert_body_contains "$_QA_LAST_BODY" '"amount":150' "orders_acme 150"
assert_body_not_contains "$_QA_LAST_BODY" '"amount":200' "orders_acme no 200"

echo "-- 4. beta user sees only beta rows"
curl_json GET "/orders" -H "Authorization: Bearer ${JWT_BETA}"
assert_status "$_QA_LAST_STATUS" "200" "orders_beta"
assert_body_contains "$_QA_LAST_BODY" '"tenant_id":"beta"' "orders_beta has beta"
assert_body_not_contains "$_QA_LAST_BODY" '"tenant_id":"acme"' "orders_beta no acme"
assert_body_contains "$_QA_LAST_BODY" '"amount":200' "orders_beta 200"

echo "-- 5. admin sees all rows"
curl_json GET "/orders" -H "Authorization: Bearer ${JWT_ADMIN}"
assert_status "$_QA_LAST_STATUS" "200" "orders_admin"
assert_body_contains "$_QA_LAST_BODY" '"tenant_id":"acme"' "orders_admin acme"
assert_body_contains "$_QA_LAST_BODY" '"tenant_id":"beta"' "orders_admin beta"
assert_body_contains "$_QA_LAST_BODY" '"amount":200' "orders_admin 200"

echo "-- 6. Masking: acme user sees *** for email"
curl_json GET "/users" -H "Authorization: Bearer ${JWT_ACME}"
assert_status "$_QA_LAST_STATUS" "200" "users_acme"
assert_body_contains "$_QA_LAST_BODY" '"email":"***"' "users_acme masked"
assert_body_not_contains "$_QA_LAST_BODY" 'alice@acme.test' "users_acme no raw email"
assert_body_not_contains "$_QA_LAST_BODY" 'bob@beta.test' "users_acme no beta row"

echo "-- 7. Masking: admin sees raw email"
curl_json GET "/users" -H "Authorization: Bearer ${JWT_ADMIN}"
assert_status "$_QA_LAST_STATUS" "200" "users_admin"
assert_body_contains "$_QA_LAST_BODY" 'alice@acme.test' "users_admin raw acme"
assert_body_contains "$_QA_LAST_BODY" 'bob@beta.test' "users_admin raw beta"
assert_body_not_contains "$_QA_LAST_BODY" '"email":"***"' "users_admin not masked"

echo "policy.test.sh OK"
stop_quackapi
