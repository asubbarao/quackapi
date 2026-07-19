#!/usr/bin/env bash
# HTTP integration: README 5-liner gate — CREATE ROUTE + serve + typed 422.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18960}"
INIT="$(mktemp /tmp/quackapi_5liner_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE hello GET '/hello' AS SELECT 'world' AS msg;
CREATE ROUTE item  GET '/items/:id' AS SELECT $id::INT AS id;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. GET /hello → [{\"msg\":\"world\"}]"
curl_json GET "/hello"
assert_status "$_QA_LAST_STATUS" "200" "hello"
assert_body_contains "$_QA_LAST_BODY" '"msg":"world"' "hello body"

echo "-- 2. GET /items/42 → typed id"
curl_json GET "/items/42"
assert_status "$_QA_LAST_STATUS" "200" "item_int"
assert_body_contains "$_QA_LAST_BODY" '"id":42' "item id"

echo "-- 3. GET /items/abc → 422 FastAPI shape"
curl_json GET "/items/abc"
assert_status "$_QA_LAST_STATUS" "422" "item_422"
assert_body_contains "$_QA_LAST_BODY" '"loc":["path","id"]' "422 loc"
assert_body_contains "$_QA_LAST_BODY" '"msg"' "422 msg"
assert_body_contains "$_QA_LAST_BODY" '"type"' "422 type"
assert_body_contains "$_QA_LAST_BODY" 'detail' "422 detail"

echo "fiveliner.test.sh OK"
stop_quackapi
