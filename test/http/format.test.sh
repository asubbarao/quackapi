#!/usr/bin/env bash
# HTTP integration: FORMAT ndjson/csv + Accept negotiation.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18971}"
INIT="$(mktemp /tmp/quackapi_format_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE items GET '/items' AS
  SELECT * FROM (VALUES (1, 'alice'), (2, 'bob,jr')) AS t(id, name);

CREATE ROUTE items_ndjson GET '/items_ndjson' FORMAT ndjson AS
  SELECT * FROM (VALUES (1, 'alice'), (2, 'bob')) AS t(id, name);

CREATE ROUTE items_csv GET '/items_csv' FORMAT csv AS
  SELECT * FROM (VALUES (1, 'alice'), (2, 'bob,jr')) AS t(id, name);

CREATE ROUTE items_json_fmt GET '/items_json_fmt' FORMAT json AS
  SELECT 1 AS id, 'x' AS name;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

header_ct() {
  echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -i '^Content-Type:' | head -1
}

echo "-- 1. default route still JSON array"
curl_json GET "/items"
assert_status "$_QA_LAST_STATUS" "200" "items_json"
assert_body_contains "$_QA_LAST_BODY" '"id":1' "items_json id"
assert_body_contains "$_QA_LAST_BODY" '[' "items_json array"
if ! header_ct | grep -qi 'application/json'; then
  echo "ASSERT FAIL: Content-Type not application/json: $(header_ct)" >&2
  exit 1
fi

echo "-- 2. Accept: text/csv on default FORMAT → CSV"
curl_json GET "/items" -H "Accept: text/csv"
assert_status "$_QA_LAST_STATUS" "200" "items_accept_csv"
assert_body_contains "$_QA_LAST_BODY" 'id,name' "csv header"
assert_body_contains "$_QA_LAST_BODY" '1,alice' "csv row1"
assert_body_contains "$_QA_LAST_BODY" '"bob,jr"' "csv escaped comma"
if ! header_ct | grep -qi 'text/csv'; then
  echo "ASSERT FAIL: Content-Type not text/csv: $(header_ct)" >&2
  exit 1
fi

echo "-- 3. Accept: application/x-ndjson on default FORMAT → NDJSON"
curl_json GET "/items" -H "Accept: application/x-ndjson"
assert_status "$_QA_LAST_STATUS" "200" "items_accept_ndjson"
assert_body_contains "$_QA_LAST_BODY" '{"id":1,"name":"alice"}' "ndjson row1"
assert_body_not_contains "$_QA_LAST_BODY" '[' "ndjson not array"
if ! header_ct | grep -qi 'application/x-ndjson'; then
  echo "ASSERT FAIL: Content-Type not application/x-ndjson: $(header_ct)" >&2
  exit 1
fi

echo "-- 4. Accept: application/jsonl also → NDJSON"
curl_json GET "/items" -H "Accept: application/jsonl"
assert_status "$_QA_LAST_STATUS" "200" "items_accept_jsonl"
assert_body_contains "$_QA_LAST_BODY" '{"id":2,"name":"bob,jr"}' "jsonl row2"
if ! header_ct | grep -qi 'application/x-ndjson'; then
  echo "ASSERT FAIL: Content-Type not application/x-ndjson for jsonl: $(header_ct)" >&2
  exit 1
fi

echo "-- 5. FORMAT ndjson route (explicit; Accept ignored for format)"
curl_json GET "/items_ndjson" -H "Accept: application/json"
assert_status "$_QA_LAST_STATUS" "200" "fmt_ndjson"
assert_body_contains "$_QA_LAST_BODY" '{"id":1,"name":"alice"}' "fmt_ndjson body"
if ! header_ct | grep -qi 'application/x-ndjson'; then
  echo "ASSERT FAIL: explicit FORMAT ndjson Content-Type: $(header_ct)" >&2
  exit 1
fi

echo "-- 6. FORMAT csv route (explicit wins over Accept: application/json)"
curl_json GET "/items_csv" -H "Accept: application/json"
assert_status "$_QA_LAST_STATUS" "200" "fmt_csv"
assert_body_contains "$_QA_LAST_BODY" 'id,name' "fmt_csv header"
assert_body_contains "$_QA_LAST_BODY" '"bob,jr"' "fmt_csv escaped"
if ! header_ct | grep -qi 'text/csv'; then
  echo "ASSERT FAIL: explicit FORMAT csv Content-Type: $(header_ct)" >&2
  exit 1
fi

echo "-- 7. FORMAT json explicit still JSON (Accept: text/csv still negotiates)"
curl_json GET "/items_json_fmt" -H "Accept: text/csv"
assert_status "$_QA_LAST_STATUS" "200" "fmt_json_accept_csv"
assert_body_contains "$_QA_LAST_BODY" 'id,name' "fmt_json negotiates csv"
if ! header_ct | grep -qi 'text/csv'; then
  echo "ASSERT FAIL: FORMAT json should honor Accept text/csv: $(header_ct)" >&2
  exit 1
fi

echo "format.test.sh OK"
stop_quackapi
