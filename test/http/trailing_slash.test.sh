#!/usr/bin/env bash
# HTTP integration: Starlette trailing-slash policy — 307 redirect to registered form.
# INTENTIONAL: match Starlette redirect_slashes (307), not silent serve or bare 404.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18955}"
INIT="$(mktemp /tmp/quackapi_slash_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE TABLE users AS
SELECT * FROM (VALUES (1, 'alice', 30), (2, 'bob', 25)) t(id, name, age);

CREATE ROUTE list_users GET '/users' AS
SELECT id, name, age FROM users ORDER BY id;

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. list_users_trailing_slash: GET /users/ → 307 Location: /users"
curl_json GET "/users/"
assert_status "$_QA_LAST_STATUS" "307" "list_users_trailing_slash"
# curl -D may leave CR; match path form (or absolute ending in /users)
if ! echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -qiE '^Location:[[:space:]]*/users[[:space:]]*$'; then
  if ! echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -qiE '^Location:[[:space:]].*/users[[:space:]]*$'; then
    echo "ASSERT FAIL: Location not /users" >&2
    echo "  headers: $_QA_LAST_HEADERS" >&2
    exit 1
  fi
fi

echo "-- 2. health_trailing_slash: GET /health/ → 307 Location: /health"
curl_json GET "/health/"
assert_status "$_QA_LAST_STATUS" "307" "health_trailing_slash"
if ! echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -qiE '^Location:[[:space:]]*/health[[:space:]]*$'; then
  if ! echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -qiE '^Location:[[:space:]].*/health[[:space:]]*$'; then
    echo "ASSERT FAIL: Location not /health" >&2
    echo "  headers: $_QA_LAST_HEADERS" >&2
    exit 1
  fi
fi

echo "-- 3. non-slash form still serves 200"
curl_json GET "/users"
assert_status "$_QA_LAST_STATUS" "200" "list_users_ok"
assert_body_contains "$_QA_LAST_BODY" 'alice' "list_users body"

curl_json GET "/health"
assert_status "$_QA_LAST_STATUS" "200" "health_ok"

echo "-- 4. unknown path with slash still 404 (no alt route)"
curl_json GET "/nope/"
assert_status "$_QA_LAST_STATUS" "404" "unknown_slash_404"

echo "trailing_slash.test.sh OK"
stop_quackapi
