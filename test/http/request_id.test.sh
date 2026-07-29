#!/usr/bin/env bash
# HTTP integration: X-Request-ID + $request_id bind (TASK.md acceptance).
# - Response always has X-Request-ID
# - Client-supplied id preserved (round-trip header + SQL bind)
# - Missing/empty → server-minted uuidv7
# - Case-insensitive request header (X-Request-Id)
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18993}"
INIT="$(mktemp /tmp/quackapi_request_id.XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE echo_rid GET '/echo-rid' AS
  SELECT $request_id::VARCHAR AS id;

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

extract_rid() {
  echo "$_QA_LAST_HEADERS" | awk 'BEGIN{IGNORECASE=1} /^X-Request-ID:/ {print $2}' | tr -d '\r'
}

echo "-- 1. Generated X-Request-ID always present; \$request_id binds"
curl_json GET "/echo-rid"
assert_status "$_QA_LAST_STATUS" "200" "gen status"
if ! echo "$_QA_LAST_HEADERS" | grep -qi 'X-Request-ID:'; then
  echo "ASSERT FAIL: X-Request-ID missing on response" >&2
  echo "  headers: $_QA_LAST_HEADERS" >&2
  exit 1
fi
RID_GEN="$(extract_rid)"
if [[ -z "$RID_GEN" ]]; then
  echo "ASSERT FAIL: empty X-Request-ID" >&2
  exit 1
fi
assert_body_contains "$_QA_LAST_BODY" "\"id\":\"${RID_GEN}\"" "\$request_id == header"
echo "   generated rid=$RID_GEN (header matches body)"

echo "-- 2. Client X-Request-ID preserved (header round-trip + SQL bind)"
CLIENT_RID="client-rid-deadbeef"
curl_json GET "/echo-rid" -H "X-Request-ID: ${CLIENT_RID}"
assert_status "$_QA_LAST_STATUS" "200" "client honor status"
assert_body_contains "$_QA_LAST_BODY" "\"id\":\"${CLIENT_RID}\"" "\$request_id bind"
OUT_RID="$(extract_rid)"
if [[ "$OUT_RID" != "$CLIENT_RID" ]]; then
  echo "ASSERT FAIL: response X-Request-ID not echoed (got='$OUT_RID' want='$CLIENT_RID')" >&2
  exit 1
fi
echo "   client rid honored"

echo "-- 3. Case-insensitive request header (X-Request-Id)"
curl_json GET "/echo-rid" -H "X-Request-Id: mixed-case-id-99"
assert_status "$_QA_LAST_STATUS" "200" "mixed case status"
assert_body_contains "$_QA_LAST_BODY" '"id":"mixed-case-id-99"' "mixed case body"
OUT_MIX="$(extract_rid)"
if [[ "$OUT_MIX" != "mixed-case-id-99" ]]; then
  echo "ASSERT FAIL: mixed-case header not honored (got='$OUT_MIX')" >&2
  exit 1
fi
echo "   X-Request-Id honored"

echo "-- 4. Empty / control-only header → mint new id (not empty)"
curl_json GET "/echo-rid" -H $'X-Request-ID: \t\r'
assert_status "$_QA_LAST_STATUS" "200" "empty control status"
RID_EMPTY="$(extract_rid)"
if [[ -z "$RID_EMPTY" ]]; then
  echo "ASSERT FAIL: expected minted id for empty/control header" >&2
  exit 1
fi
# Body must match header
assert_body_contains "$_QA_LAST_BODY" "\"id\":\"${RID_EMPTY}\"" "mint after empty"
echo "   empty/control → minted $RID_EMPTY"

echo "-- 5. Overlong client id truncated to 128 printable chars"
# 140 'a's → first 128 kept
LONG="$(printf 'a%.0s' {1..140})"
curl_json GET "/echo-rid" -H "X-Request-ID: ${LONG}"
assert_status "$_QA_LAST_STATUS" "200" "truncate status"
OUT_LONG="$(extract_rid)"
if [[ ${#OUT_LONG} -ne 128 ]]; then
  echo "ASSERT FAIL: expected 128-char rid, got len=${#OUT_LONG}" >&2
  exit 1
fi
WANT_LONG="$(printf 'a%.0s' {1..128})"
if [[ "$OUT_LONG" != "$WANT_LONG" ]]; then
  echo "ASSERT FAIL: truncate content mismatch" >&2
  exit 1
fi
assert_body_contains "$_QA_LAST_BODY" "\"id\":\"${WANT_LONG}\"" "truncated bind"
echo "   overlong truncated to 128"

echo "-- 6. Per-request uniqueness when not client-supplied"
curl_json GET "/echo-rid"
RID_A="$(extract_rid)"
curl_json GET "/echo-rid"
RID_B="$(extract_rid)"
if [[ "$RID_A" == "$RID_B" ]]; then
  echo "ASSERT FAIL: successive generated ids collided ($RID_A)" >&2
  exit 1
fi
echo "   unique: $RID_A vs $RID_B"

echo "request_id.test.sh OK"
stop_quackapi
