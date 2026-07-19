#!/usr/bin/env bash
# HTTP integration: HEADER params (FastAPI Header) — wire-name normalization,
# missing → 422 loc header, case-insensitive header lookup.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18951}"
INIT="$(mktemp /tmp/quackapi_headers_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
-- x_token → header X-Token (underscore→hyphen, case-insensitive)
CREATE ROUTE header_echo GET '/header-echo'
  PARAM x_token HEADER
  AS
SELECT $x_token::VARCHAR AS token;

-- Explicit wire name
CREATE ROUTE ua GET '/ua'
  PARAM agent VARCHAR HEADER 'User-Agent'
  AS
SELECT $agent::VARCHAR AS agent;

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. header_param: X-Token binds via PARAM x_token HEADER"
curl_json GET "/header-echo" -H "X-Token: abc"
assert_status "$_QA_LAST_STATUS" "200" "header_param"
assert_body_contains "$_QA_LAST_BODY" '"token":"abc"' "header_param body"

echo "-- 2. header case-insensitive: x-token still binds"
curl_json GET "/header-echo" -H "x-token: lower"
assert_status "$_QA_LAST_STATUS" "200" "header_ci"
assert_body_contains "$_QA_LAST_BODY" '"token":"lower"' "header_ci body"

echo "-- 3. missing header → 422 loc header"
curl_json GET "/header-echo"
assert_status "$_QA_LAST_STATUS" "422" "header_missing"
assert_body_contains "$_QA_LAST_BODY" '"loc":["header","x_token"]' "header_missing loc"
assert_body_contains "$_QA_LAST_BODY" 'missing' "header_missing type"

echo "-- 4. explicit HEADER wire name User-Agent"
curl_json GET "/ua" -H "User-Agent: quackapi-test/1.0"
assert_status "$_QA_LAST_STATUS" "200" "ua_header"
assert_body_contains "$_QA_LAST_BODY" 'quackapi-test/1.0' "ua body"

echo "headers.test.sh OK"
stop_quackapi
