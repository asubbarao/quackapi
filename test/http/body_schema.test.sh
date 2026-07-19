#!/usr/bin/env bash
# HTTP integration: BODY SCHEMA via json_schema community extension.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18942}"
INIT="$(mktemp /tmp/quackapi_body_schema_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
-- Require name (string) and age (integer) in the JSON body.
CREATE ROUTE create_user POST '/users' STATUS 201
  BODY SCHEMA '{"type":"object","required":["name","age"],"properties":{"name":{"type":"string"},"age":{"type":"integer"}},"additionalProperties":false}'
  AS
SELECT $name::VARCHAR AS name, $age::INTEGER AS age;

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. valid body passes schema + binds"
curl_json POST "/users" -H "Content-Type: application/json" --data-binary '{"name":"ada","age":42}'
assert_status "$_QA_LAST_STATUS" "201" "schema_ok"
assert_body_contains "$_QA_LAST_BODY" '"name":"ada"' "schema_ok name"
assert_body_contains "$_QA_LAST_BODY" '"age":42' "schema_ok age"

echo "-- 2. missing required field → 422 loc body"
curl_json POST "/users" -H "Content-Type: application/json" --data-binary '{"name":"ada"}'
assert_status "$_QA_LAST_STATUS" "422" "schema_missing"
assert_body_contains "$_QA_LAST_BODY" '"loc":["body"' "schema_missing loc"
assert_body_contains "$_QA_LAST_BODY" 'value_error' "schema_missing type"

echo "-- 3. wrong type for age → 422"
curl_json POST "/users" -H "Content-Type: application/json" --data-binary '{"name":"ada","age":"forty"}'
assert_status "$_QA_LAST_STATUS" "422" "schema_type"
assert_body_contains "$_QA_LAST_BODY" '"loc":["body"' "schema_type loc"

echo "-- 4. wrong Content-Type with BODY SCHEMA → 422 body"
curl_json POST "/users" -H "Content-Type: text/plain" --data-binary '{"name":"ada","age":1}'
assert_status "$_QA_LAST_STATUS" "422" "schema_wrong_ct"
assert_body_contains "$_QA_LAST_BODY" '"loc":["body"]' "schema_wrong_ct loc"

echo "body_schema.test.sh OK"
stop_quackapi
