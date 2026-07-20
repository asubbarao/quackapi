#!/usr/bin/env bash
# HTTP integration: CREATE GROUP — prefixed paths + inherited REQUIRE auth.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18971}"
INIT="$(mktemp /tmp/quackapi_group.XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE AUTH api AS API_KEY;
SELECT * FROM quackapi_add_api_key('api', 'k-g', 'u');
CREATE GROUP v1 WITH (prefix='/api/v1', auth=api, tags='items,v1');
CREATE ROUTE items_list GET '/items' GROUP v1 AS SELECT 1 AS id, 'x' AS name;
CREATE ROUTE items_get GET '/items/:id' IN GROUP v1 AS SELECT $id::INTEGER AS id;
CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- G1. grouped path without key → 401 (inherited auth)"
curl_json GET "/api/v1/items"
assert_status "$_QA_LAST_STATUS" "401" "group_unauth"

echo "-- G2. grouped path with key → 200"
curl_json GET "/api/v1/items" -H "X-API-Key: k-g"
assert_status "$_QA_LAST_STATUS" "200" "group_auth"
assert_body_contains "$_QA_LAST_BODY" '"id":1' "group_auth id"
assert_body_contains "$_QA_LAST_BODY" '"name":"x"' "group_auth name"

echo "-- G3. path param under group prefix"
curl_json GET "/api/v1/items/42" -H "X-API-Key: k-g"
assert_status "$_QA_LAST_STATUS" "200" "group_param"
assert_body_contains "$_QA_LAST_BODY" '"id":42' "group_param id"

echo "-- G4. bare relative path not mounted"
curl_json GET "/items" -H "X-API-Key: k-g"
assert_status "$_QA_LAST_STATUS" "404" "group_no_bare"

echo "-- G5. ungrouped absolute still public"
curl_json GET "/health"
assert_status "$_QA_LAST_STATUS" "200" "group_health"
assert_body_contains "$_QA_LAST_BODY" '"status":"ok"' "group_health body"

echo "-- G6. wrong key → 401"
curl_json GET "/api/v1/items" -H "X-API-Key: wrong"
assert_status "$_QA_LAST_STATUS" "401" "group_bad_key"

echo "group.test.sh OK"
stop_quackapi
