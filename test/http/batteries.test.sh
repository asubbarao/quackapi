#!/usr/bin/env bash
# HTTP integration: batteries-included defaults — /health, /healthz, X-Request-ID, access log.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18990}"
INIT="$(mktemp /tmp/quackapi_batteries_XXXXXX.sql)"

# Minimal init — batteries should not require any CREATE ROUTE for health.
cat >"$INIT" <<'SQL'
SELECT 1;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. GET /health → 200 {status:ok}"
curl_json GET "/health"
assert_status "$_QA_LAST_STATUS" "200" "health"
assert_body_contains "$_QA_LAST_BODY" '"status":"ok"' "health body"

if ! echo "$_QA_LAST_HEADERS" | grep -qi 'X-Request-ID:'; then
  echo "ASSERT FAIL: X-Request-ID missing on /health" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi
RID1="$(echo "$_QA_LAST_HEADERS" | awk 'BEGIN{IGNORECASE=1} /^X-Request-ID:/ {print $2}' | tr -d '\r')"
if [[ -z "$RID1" ]]; then
  echo "ASSERT FAIL: empty X-Request-ID" >&2
  exit 1
fi
echo "   request_id_1=$RID1"

echo "-- 2. GET /healthz → 200 readiness (version + uptime)"
curl_json GET "/healthz"
assert_status "$_QA_LAST_STATUS" "200" "healthz"
assert_body_contains "$_QA_LAST_BODY" '"status":"ok"' "healthz status"
assert_body_contains "$_QA_LAST_BODY" '"version"' "healthz version"
assert_body_contains "$_QA_LAST_BODY" '"uptime_sec"' "healthz uptime"

if ! echo "$_QA_LAST_HEADERS" | grep -qi 'X-Request-ID:'; then
  echo "ASSERT FAIL: X-Request-ID missing on /healthz" >&2
  exit 1
fi
RID2="$(echo "$_QA_LAST_HEADERS" | awk 'BEGIN{IGNORECASE=1} /^X-Request-ID:/ {print $2}' | tr -d '\r')"
if [[ -z "$RID2" ]]; then
  echo "ASSERT FAIL: empty X-Request-ID on healthz" >&2
  exit 1
fi
if [[ "$RID1" == "$RID2" ]]; then
  echo "ASSERT FAIL: X-Request-ID should differ per request (got $RID1 twice)" >&2
  exit 1
fi
echo "   request_id_2=$RID2 (differs from #1)"

echo "-- 3. Access log line emitted per request with required fields"
# boot_quackapi captures duckdb stdout+stderr to $_QA_LOG
if [[ ! -f "$_QA_LOG" ]]; then
  echo "ASSERT FAIL: server log file missing" >&2
  exit 1
fi
# Wait briefly for stderr flush
sleep 0.15
ACCESS_LINES="$(grep -E '"type":"access"' "$_QA_LOG" || true)"
if [[ -z "$ACCESS_LINES" ]]; then
  echo "ASSERT FAIL: no access-log JSON lines in server log" >&2
  echo "--- log ---" >&2
  cat "$_QA_LOG" >&2
  exit 1
fi
# At least two access lines (health + healthz); check fields on the last one
LAST_ACCESS="$(echo "$ACCESS_LINES" | tail -1)"
for field in method path status latency_ms request_id bytes; do
  if [[ "$LAST_ACCESS" != *'"'"$field"'"'* ]]; then
    echo "ASSERT FAIL: access log missing field '$field'" >&2
    echo "  line: $LAST_ACCESS" >&2
    exit 1
  fi
done
# request_id in log should match one of the responses
if [[ "$ACCESS_LINES" != *"$RID1"* ]] && [[ "$ACCESS_LINES" != *"$RID2"* ]]; then
  echo "ASSERT FAIL: access log request_id does not match X-Request-ID headers" >&2
  echo "  log: $ACCESS_LINES" >&2
  exit 1
fi
echo "   access log OK: $LAST_ACCESS"

echo "-- 4. Second /health → new X-Request-ID"
curl_json GET "/health"
assert_status "$_QA_LAST_STATUS" "200" "health2"
RID3="$(echo "$_QA_LAST_HEADERS" | awk 'BEGIN{IGNORECASE=1} /^X-Request-ID:/ {print $2}' | tr -d '\r')"
if [[ "$RID3" == "$RID1" ]] || [[ "$RID3" == "$RID2" ]]; then
  echo "ASSERT FAIL: third request_id collided with prior" >&2
  exit 1
fi

echo "batteries.test.sh OK"
stop_quackapi
