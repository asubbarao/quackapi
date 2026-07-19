#!/usr/bin/env bash
# HTTP integration: 404 / 405+Allow / HEAD auto / OPTIONS without CORS.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18962}"
INIT="$(mktemp /tmp/quackapi_routing_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
CREATE ROUTE list_users GET '/users' AS SELECT 1 AS id, 'alice' AS name;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. not_found → 404"
curl_json GET "/nope"
assert_status "$_QA_LAST_STATUS" "404" "not_found"
assert_body_contains "$_QA_LAST_BODY" 'Not Found' "not_found body"

echo "-- 2. method mismatch → 405 + Allow"
curl_json POST "/health" -H "Content-Type: application/json" --data-binary '{}'
assert_status "$_QA_LAST_STATUS" "405" "health_post_405"
assert_body_contains "$_QA_LAST_BODY" 'Method Not Allowed' "405 body"
if ! echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -qi '^Allow:'; then
  echo "ASSERT FAIL (health_post_405): Allow header missing" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi
if ! echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -qiE '^Allow:.*GET'; then
  echo "ASSERT FAIL (health_post_405): Allow should list GET" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi

echo "-- 3. DELETE /users (GET-only) → 405 + Allow"
curl_json DELETE "/users"
assert_status "$_QA_LAST_STATUS" "405" "method_mismatch_users_delete"
if ! echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -qi '^Allow:'; then
  echo "ASSERT FAIL: Allow missing on DELETE /users" >&2
  exit 1
fi

echo "-- 4. OPTIONS without CORS → 405 (FastAPI default)"
curl_json OPTIONS "/health"
assert_status "$_QA_LAST_STATUS" "405" "health_options"
if ! echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -qi '^Allow:'; then
  echo "ASSERT FAIL (health_options): Allow missing" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi

echo "-- 5. auto-HEAD for GET route → 200 empty body"
curl_json HEAD "/health"
assert_status "$_QA_LAST_STATUS" "200" "health_head_auto"
if [[ -n "${_QA_LAST_BODY}" ]]; then
  echo "ASSERT FAIL (health_head_auto): expected empty body, got: $_QA_LAST_BODY" >&2
  exit 1
fi

echo "-- 6. HEAD on path GET → 200"
curl_json HEAD "/users"
assert_status "$_QA_LAST_STATUS" "200" "get_user_head_explicit"

echo "routing.test.sh OK"
stop_quackapi
