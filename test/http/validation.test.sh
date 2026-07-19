#!/usr/bin/env bash
# HTTP integration: strict integer binding, never-500 client errors, param names,
# optional query params (PARAM … DEFAULT), and query constraints (LE/GE/…).
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18931}"
INIT="$(mktemp /tmp/quackapi_validation_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE TABLE users AS
SELECT * FROM (VALUES
  (1, 'alice', 30),
  (2, 'bob', 25),
  (3, 'carol', 40)
) t(id, name, age);

CREATE ROUTE get_user GET '/users/:id' AS
SELECT id, name, age FROM users WHERE id = $id::INTEGER;

CREATE ROUTE create_user POST '/users' STATUS 201 AS
SELECT $name::VARCHAR AS name, $age::INTEGER AS age;

-- Optional limit (DEFAULT 10) + LE 100 constraint — FastAPI Query(10, le=100)
CREATE ROUTE search_limit GET '/search_limit'
  PARAM limit INTEGER DEFAULT 10 LE 100
  AS
SELECT id, name, age FROM users
WHERE name ILIKE $q::VARCHAR || '%'
ORDER BY id
LIMIT $limit::INTEGER;

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. strict int: path float 1.5 → 422 (get_user_bad_float)"
curl_json GET "/users/1.5"
assert_status "$_QA_LAST_STATUS" "422" "get_user_bad_float"
assert_body_contains "$_QA_LAST_BODY" '"loc":["path","id"]' "get_user_bad_float loc"
assert_body_contains "$_QA_LAST_BODY" 'type_error' "get_user_bad_float type"

echo "-- 2. strict int: query age float str → 422 (post_users_age_float_str)"
curl_json POST "/users?name=x&age=1.5" -H "Content-Type: application/json" --data-binary '{}'
assert_status "$_QA_LAST_STATUS" "422" "post_users_age_float_str"
assert_body_contains "$_QA_LAST_BODY" '"loc":["query","age"]' "post age loc"

echo "-- 3. strict int: limit=1.5 → 422 (search_limit_float)"
curl_json GET "/search_limit?q=a&limit=1.5"
assert_status "$_QA_LAST_STATUS" "422" "search_limit_float"
assert_body_contains "$_QA_LAST_BODY" '"loc":["query","limit"]' "limit float loc"

echo "-- 4. strict int: limit=1e2 → 422 (search_limit_1e2)"
curl_json GET "/search_limit?q=a&limit=1e2"
assert_status "$_QA_LAST_STATUS" "422" "search_limit_1e2"
assert_body_contains "$_QA_LAST_BODY" '"loc":["query","limit"]' "limit 1e2 loc"

echo "-- 5. never 500: limit=-1 → 200 [] (search_limit_neg)"
curl_json GET "/search_limit?q=a&limit=-1"
assert_status "$_QA_LAST_STATUS" "200" "search_limit_neg"
assert_body_contains "$_QA_LAST_BODY" '[]' "search_limit_neg body"

echo "-- 6. param name on conversion: limit=abc → 422 loc limit (search_limit_bad_int)"
curl_json GET "/search_limit?q=a&limit=abc"
assert_status "$_QA_LAST_STATUS" "422" "search_limit_bad_int"
assert_body_contains "$_QA_LAST_BODY" '"loc":["query","limit"]' "bad_int loc name"
assert_body_not_contains "$_QA_LAST_BODY" '"loc":["query","_"]' "bad_int not underscore"

echo "-- 7. optional query: missing limit → 200 with default (search_limit_missing)"
curl_json GET "/search_limit?q=a"
assert_status "$_QA_LAST_STATUS" "200" "search_limit_missing"
assert_body_contains "$_QA_LAST_BODY" 'alice' "search_limit_missing uses default"

echo "-- 8. constraint LE: limit=101 → 422 less_than_equal (search_limit_le)"
curl_json GET "/search_limit?q=a&limit=101"
assert_status "$_QA_LAST_STATUS" "422" "search_limit_le"
assert_body_contains "$_QA_LAST_BODY" '"loc":["query","limit"]' "le loc"
assert_body_contains "$_QA_LAST_BODY" 'less_than_equal' "le type"

echo "-- 9. happy paths still work"
curl_json GET "/users/1"
assert_status "$_QA_LAST_STATUS" "200" "get_user_1"
assert_body_contains "$_QA_LAST_BODY" 'alice' "get_user_1 body"

curl_json GET "/search_limit?q=a&limit=2"
assert_status "$_QA_LAST_STATUS" "200" "search_limit_happy"

curl_json GET "/users/01"
assert_status "$_QA_LAST_STATUS" "200" "leading_zero still ok"

echo "validation.test.sh OK"
stop_quackapi
