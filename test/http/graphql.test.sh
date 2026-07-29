#!/usr/bin/env bash
# HTTP integration: thin GraphQL v0 — create table, insert, POST /graphql, schema.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18990}"
INIT="$(mktemp /tmp/quackapi_graphql_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE TABLE cases AS
SELECT 1 AS id, '24-000117' AS case_no
UNION ALL
SELECT 2 AS id, '24-000200' AS case_no;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. POST /graphql table selection"
curl_json POST "/graphql" \
  -H "Content-Type: application/json" \
  -d '{"query":"{ cases { id case_no } }"}'
assert_status "$_QA_LAST_STATUS" "200" "gql_select"
assert_body_contains "$_QA_LAST_BODY" '"data"' "has data"
assert_body_contains "$_QA_LAST_BODY" '"cases"' "has cases"
assert_body_contains "$_QA_LAST_BODY" '"id":1' "row1 id"
assert_body_contains "$_QA_LAST_BODY" '"case_no":"24-000117"' "row1 case_no"
assert_body_contains "$_QA_LAST_BODY" '"id":2' "row2 id"
assert_body_not_contains "$_QA_LAST_BODY" '"errors"' "no errors on happy path"

echo "-- 2. POST with query keyword + column subset"
curl_json POST "/graphql" \
  -H "Content-Type: application/json" \
  -d '{"query":"query { cases { id } }"}'
assert_status "$_QA_LAST_STATUS" "200" "gql_query_kw"
assert_body_contains "$_QA_LAST_BODY" '"id":1' "subset id"
assert_body_not_contains "$_QA_LAST_BODY" 'case_no' "subset drops case_no"

echo "-- 3. unknown table → errors"
curl_json POST "/graphql" \
  -H "Content-Type: application/json" \
  -d '{"query":"{ missing_table { id } }"}'
assert_status "$_QA_LAST_STATUS" "200" "gql_unknown_table"
assert_body_contains "$_QA_LAST_BODY" '"errors"' "errors key"
assert_body_contains "$_QA_LAST_BODY" 'unknown table' "unknown table msg"

echo "-- 4. unknown column → errors"
curl_json POST "/graphql" \
  -H "Content-Type: application/json" \
  -d '{"query":"{ cases { not_a_col } }"}'
assert_status "$_QA_LAST_STATUS" "200" "gql_unknown_col"
assert_body_contains "$_QA_LAST_BODY" 'unknown column' "unknown col msg"

echo "-- 5. bad body → 400 GraphQL-ish errors"
curl_json POST "/graphql" \
  -H "Content-Type: application/json" \
  -d '{"nope":true}'
assert_status "$_QA_LAST_STATUS" "400" "gql_bad_body"
assert_body_contains "$_QA_LAST_BODY" '"errors"' "bad body errors"

echo "-- 6. GET /graphql/schema"
curl_json GET "/graphql/schema"
assert_status "$_QA_LAST_STATUS" "200" "gql_schema"
assert_body_contains "$_QA_LAST_BODY" '"tables"' "schema tables"
assert_body_contains "$_QA_LAST_BODY" '"cases"' "schema cases"
assert_body_contains "$_QA_LAST_BODY" '"id"' "schema id col"
assert_body_contains "$_QA_LAST_BODY" '"case_no"' "schema case_no col"

echo "-- 7. GET /graphql → 405"
curl_json GET "/graphql"
assert_status "$_QA_LAST_STATUS" "405" "gql_get_405"

echo "graphql.test.sh OK"
stop_quackapi
