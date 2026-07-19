#!/usr/bin/env bash
# HTTP integration: OPTIONS preflight + CORS headers when cors_origins is set.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18963}"
INIT="$(mktemp /tmp/quackapi_cors_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SET quackapi_cors_origins = '*';
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. OPTIONS with CORS on → 204 + Access-Control-Allow-Origin"
curl_json OPTIONS "/health" \
  -H "Origin: https://app.example" \
  -H "Access-Control-Request-Method: GET"
assert_status "$_QA_LAST_STATUS" "204" "options_cors_preflight"
if ! echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -qi '^Access-Control-Allow-Origin:'; then
  echo "ASSERT FAIL: Access-Control-Allow-Origin missing" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi
if ! echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -qi '^Allow:'; then
  echo "ASSERT FAIL: Allow missing on CORS OPTIONS" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi

echo "-- 2. GET with Origin still gets CORS header"
curl_json GET "/health" -H "Origin: https://app.example"
assert_status "$_QA_LAST_STATUS" "200" "get_with_origin"
if ! echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -qi '^Access-Control-Allow-Origin:'; then
  echo "ASSERT FAIL: CORS header missing on GET" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi

echo "cors.test.sh OK"
stop_quackapi
