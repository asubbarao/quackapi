#!/usr/bin/env bash
# HTTP integration: built-in /redoc (ReDoc HTML over /openapi.json).
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18954}"
INIT="$(mktemp /tmp/quackapi_redoc_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. redoc_get: 200 HTML with redoc + openapi.json"
curl_json GET "/redoc"
assert_status "$_QA_LAST_STATUS" "200" "redoc_get"
assert_body_contains "$_QA_LAST_BODY" 'redoc' "redoc tag"
assert_body_contains "$_QA_LAST_BODY" '/openapi.json' "redoc spec-url"
if ! echo "$_QA_LAST_HEADERS" | grep -qi 'Content-Type: *text/html'; then
  echo "ASSERT FAIL (redoc_get): Content-Type not text/html" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi

echo "-- 2. /docs still works (sibling)"
curl_json GET "/docs"
assert_status "$_QA_LAST_STATUS" "200" "docs_get"
assert_body_contains "$_QA_LAST_BODY" 'swagger' "docs swagger"

echo "-- 3. /openapi.json has openapi key"
curl_json GET "/openapi.json"
assert_status "$_QA_LAST_STATUS" "200" "openapi_json"
assert_body_contains "$_QA_LAST_BODY" '"openapi"' "openapi key"

echo "redoc.test.sh OK"
stop_quackapi
