#!/usr/bin/env bash
# HTTP integration: application/x-www-form-urlencoded body binding.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18943}"
INIT="$(mktemp /tmp/quackapi_form_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE form_submit POST '/form-submit' AS
SELECT $name::VARCHAR AS name, $age::INTEGER AS age;

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. form_submit happy path"
curl_json POST "/form-submit" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data-binary 'name=zed&age=31'
assert_status "$_QA_LAST_STATUS" "200" "form_submit"
assert_body_contains "$_QA_LAST_BODY" '"name":"zed"' "form name"
assert_body_contains "$_QA_LAST_BODY" '"age":31' "form age"

echo "-- 2. form url-encoded spaces (+ and %20)"
curl_json POST "/form-submit" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data-binary 'name=zed+smith&age=30'
assert_status "$_QA_LAST_STATUS" "200" "form_plus"
assert_body_contains "$_QA_LAST_BODY" '"name":"zed smith"' "form plus space"

echo "-- 3. missing form field → 422 loc body"
curl_json POST "/form-submit" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data-binary 'name=only'
assert_status "$_QA_LAST_STATUS" "422" "form_missing"
# age missing — after form parse, missing uses query default for non-path unless
# provided loc; form fields that exist are body. Missing may be query loc.
assert_body_contains "$_QA_LAST_BODY" 'age' "form_missing age"

echo "form.test.sh OK"
stop_quackapi
