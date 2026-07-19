#!/usr/bin/env bash
# HTTP integration: RedirectResponse via STATUS 3xx + location column.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18953}"
INIT="$(mktemp /tmp/quackapi_redirect_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE old_home GET '/old-home' STATUS 307 AS
SELECT '/new-home' AS location;

CREATE ROUTE moved GET '/moved' STATUS 301 AS
SELECT 'https://example.com/new' AS location, 'gone' AS note;

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. redirect_307: 307 + Location: /new-home, empty body"
curl_json GET "/old-home"
assert_status "$_QA_LAST_STATUS" "307" "redirect_307"
if ! echo "$_QA_LAST_HEADERS" | grep -qi '^Location: */new-home'; then
  echo "ASSERT FAIL (redirect_307): Location header missing or wrong" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi
# body should be empty (location-only control column)
if [[ -n "${_QA_LAST_BODY}" ]]; then
  echo "ASSERT FAIL (redirect_307): expected empty body, got: $_QA_LAST_BODY" >&2
  exit 1
fi

echo "-- 2. redirect with extra data column still sets Location"
curl_json GET "/moved"
assert_status "$_QA_LAST_STATUS" "301" "redirect_301"
if ! echo "$_QA_LAST_HEADERS" | grep -qi '^Location: *https://example.com/new'; then
  echo "ASSERT FAIL (redirect_301): Location wrong" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi
assert_body_contains "$_QA_LAST_BODY" '"note":"gone"' "redirect_301 body note"
assert_body_not_contains "$_QA_LAST_BODY" 'location' "location stripped from body"

echo "redirect.test.sh OK"
stop_quackapi
