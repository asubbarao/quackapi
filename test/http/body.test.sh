#!/usr/bin/env bash
# HTTP integration: JSON request body binder — happy path, malformed 422,
# wrong Content-Type 422, loc=["body", ...].
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18941}"
INIT="$(mktemp /tmp/quackapi_body_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE create_user POST '/users' STATUS 201 AS
SELECT $name::VARCHAR AS name, $age::INTEGER AS age;

CREATE ROUTE echo_body POST '/echo-body' AS
SELECT $body::VARCHAR AS body;

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. post_users_json_body: JSON fields bind as params"
curl_json POST "/users" -H "Content-Type: application/json" --data-binary '{"name":"dave","age":35}'
assert_status "$_QA_LAST_STATUS" "201" "post_users_json_body"
assert_body_contains "$_QA_LAST_BODY" '"name":"dave"' "json body name"
assert_body_contains "$_QA_LAST_BODY" '"age":35' "json body age"

echo "-- 2. post_users_malformed_json: invalid JSON → 422 loc body json_invalid"
curl_json POST "/users" -H "Content-Type: application/json" --data-binary '{not json}'
assert_status "$_QA_LAST_STATUS" "422" "post_users_malformed_json"
assert_body_contains "$_QA_LAST_BODY" '"loc":["body"]' "malformed loc"
assert_body_contains "$_QA_LAST_BODY" 'json_invalid' "malformed type"

echo "-- 3. post_users_wrong_ct: text/plain → 422 model_attributes_type on body"
curl_json POST "/users" -H "Content-Type: text/plain" --data-binary '{"name":"x","age":5}'
assert_status "$_QA_LAST_STATUS" "422" "post_users_wrong_ct"
assert_body_contains "$_QA_LAST_BODY" '"loc":["body"]' "wrong_ct loc"
assert_body_contains "$_QA_LAST_BODY" 'model_attributes' "wrong_ct type"
# Driver also requires "name" not appear as a field error key in the body text
assert_body_not_contains "$_QA_LAST_BODY" '"name"' "wrong_ct no name field"

echo "-- 4. query still wins when present (hybrid binder)"
curl_json POST "/users?name=fromq&age=9" -H "Content-Type: application/json" --data-binary '{"name":"fromb","age":99}'
assert_status "$_QA_LAST_STATUS" "201" "query_wins"
assert_body_contains "$_QA_LAST_BODY" '"name":"fromq"' "query wins name"
assert_body_contains "$_QA_LAST_BODY" '"age":9' "query wins age"

echo "-- 5. body type error loc=body"
curl_json POST "/users" -H "Content-Type: application/json" --data-binary '{"name":"x","age":"nope"}'
assert_status "$_QA_LAST_STATUS" "422" "body_type_error"
assert_body_contains "$_QA_LAST_BODY" '"loc":["body","age"]' "body type loc"

echo "-- 6. \$body raw payload"
curl_json POST "/echo-body" -H "Content-Type: application/json" --data-binary '{"k":1}'
assert_status "$_QA_LAST_STATUS" "200" "echo_body"
assert_body_contains "$_QA_LAST_BODY" '{\"k\":1}' "raw body"

echo "body.test.sh OK"
stop_quackapi
