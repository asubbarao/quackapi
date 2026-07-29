#!/usr/bin/env bash
# RATE LIMIT <n> PER <s>: under limit 200; over limit 429 + Retry-After
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18991}"
INIT="$(mktemp /tmp/quackapi_rate_limit.XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE limited GET '/limited' RATE LIMIT 3 PER 60 BY ip AS
  SELECT 'ok' AS msg;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. three requests under limit → 200"
for i in 1 2 3; do
  curl_json GET "/limited"
  assert_status "$_QA_LAST_STATUS" "200" "under_$i"
  assert_body_contains "$_QA_LAST_BODY" 'ok' "under_$i body"
done

echo "-- 2. fourth request → 429 + Retry-After"
curl_json GET "/limited"
assert_status "$_QA_LAST_STATUS" "429" "over_limit"
assert_body_contains "$_QA_LAST_BODY" 'Rate limit exceeded' "429 body"
if ! echo "$_QA_LAST_HEADERS" | grep -qi 'Retry-After:'; then
  echo "ASSERT FAIL: missing Retry-After header" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi

echo "rate_limit.test.sh OK"
