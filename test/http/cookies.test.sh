#!/usr/bin/env bash
# HTTP integration: COOKIE params (FastAPI Cookie) + Set-Cookie response column.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18952}"
INIT="$(mktemp /tmp/quackapi_cookies_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE profile GET '/profile'
  PARAM session COOKIE
  AS
SELECT $session::VARCHAR AS session;

CREATE ROUTE login POST '/login' AS
SELECT 'session=sess-abc; Path=/' AS set_cookie, true AS ok;

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. cookie_param: Cookie session=sess-abc binds"
curl_json GET "/profile" -H "Cookie: session=sess-abc"
assert_status "$_QA_LAST_STATUS" "200" "cookie_param"
assert_body_contains "$_QA_LAST_BODY" '"session":"sess-abc"' "cookie_param body"

echo "-- 2. multi-cookie: extract named cookie"
curl_json GET "/profile" -H "Cookie: other=1; session=xyz; flag=true"
assert_status "$_QA_LAST_STATUS" "200" "cookie_multi"
assert_body_contains "$_QA_LAST_BODY" '"session":"xyz"' "cookie_multi body"

echo "-- 3. missing cookie → 422 loc cookie"
curl_json GET "/profile"
assert_status "$_QA_LAST_STATUS" "422" "cookie_missing"
assert_body_contains "$_QA_LAST_BODY" '"loc":["cookie","session"]' "cookie_missing loc"

echo "-- 4. set_cookie: response Set-Cookie header + body without set_cookie col"
curl_json POST "/login" -H "Content-Type: application/json" --data-binary '{}'
assert_status "$_QA_LAST_STATUS" "200" "set_cookie"
if ! echo "$_QA_LAST_HEADERS" | grep -qi '^Set-Cookie:'; then
  echo "ASSERT FAIL (set_cookie): Set-Cookie header missing" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi
assert_body_contains "$_QA_LAST_BODY" '"ok":true' "set_cookie body ok"
assert_body_not_contains "$_QA_LAST_BODY" 'set_cookie' "set_cookie stripped from body"
assert_body_contains "$_QA_LAST_HEADERS" 'sess-abc' "set_cookie value"

echo "cookies.test.sh OK"
stop_quackapi
