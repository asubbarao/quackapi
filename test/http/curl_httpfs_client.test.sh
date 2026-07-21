#!/usr/bin/env bash
# HTTP integration: batteries outbound HTTP client — curl_httpfs preferred,
# graceful httplib fallback. Platform-aware: asserts 'curl' when curl_httpfs
# INSTALL/LOAD succeeds on this machine, else 'httplib'.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18991}"
PORT_FORCE="${QUACKAPI_TEST_PORT_FORCE:-18992}"

# --- Probe: is curl_httpfs available on THIS platform/environment? ---
# Same path batteries uses (INSTALL FROM community + LOAD). Truth, not guess.
EXPECTED_CLIENT="httplib"
PROBE_LOG="$(mktemp /tmp/quackapi_curl_probe_XXXXXX.log)"
set +e
"$DUCKDB_BIN" -unsigned -c "
INSTALL curl_httpfs FROM community;
LOAD curl_httpfs;
SELECT 1;
" >"$PROBE_LOG" 2>&1
PROBE_RC=$?
set -e
if [[ $PROBE_RC -eq 0 ]]; then
  EXPECTED_CLIENT="curl"
fi
echo "probe: curl_httpfs available → expect healthz http_client=$EXPECTED_CLIENT (probe_rc=$PROBE_RC)"
if [[ $PROBE_RC -ne 0 ]]; then
  echo "probe log (truncated):"
  tail -20 "$PROBE_LOG" || true
fi
rm -f "$PROBE_LOG"

# Stable public JSON (GitHub raw) — small, long-lived, https.
REMOTE_URL='https://raw.githubusercontent.com/dentiny/duck-read-cache-fs/main/test/data/stock-exchanges.csv'

INIT="$(mktemp /tmp/quackapi_curl_httpfs_XXXXXX.sql)"
# CREATE ROUTE validates handler SQL at registration time, so httpfs must be
# loadable before serve. Batteries still prefer curl_httpfs on quackapi_serve().
cat >"$INIT" <<SQL
INSTALL httpfs;
LOAD httpfs;
CREATE ROUTE remote_csv GET '/remote' AS
SELECT length(content) AS n FROM read_text('${REMOTE_URL}');
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. /healthz reports http_client matching platform probe"
curl_json GET "/healthz"
assert_status "$_QA_LAST_STATUS" "200" "healthz"
assert_body_contains "$_QA_LAST_BODY" '"status":"ok"' "healthz status"
assert_body_contains "$_QA_LAST_BODY" "\"http_client\":\"${EXPECTED_CLIENT}\"" "healthz http_client"

echo "-- 2. Route that reads remote https succeeds (either client backend)"
curl_json GET "/remote"
assert_status "$_QA_LAST_STATUS" "200" "remote"
# stock-exchanges.csv is ~16k; length is a positive int in JSON array/object
if [[ "$_QA_LAST_BODY" != *'"n"'* ]] && [[ "$_QA_LAST_BODY" != *'16205'* ]] && [[ "$_QA_LAST_BODY" != *[0-9]* ]]; then
  echo "ASSERT FAIL: remote body missing numeric length: $_QA_LAST_BODY" >&2
  exit 1
fi
# Must not be an error object
if [[ "$_QA_LAST_BODY" == *'"detail"'* ]] && [[ "$_QA_LAST_BODY" == *'error'* ]]; then
  echo "ASSERT FAIL: remote fetch looks like error: $_QA_LAST_BODY" >&2
  exit 1
fi
echo "   remote body: $_QA_LAST_BODY"

echo "-- 3. Server log records chosen client"
if [[ ! -f "$_QA_LOG" ]]; then
  echo "ASSERT FAIL: server log missing" >&2
  exit 1
fi
sleep 0.1
if [[ "$EXPECTED_CLIENT" == "curl" ]]; then
  if ! grep -q 'quackapi.http_client=curl' "$_QA_LOG"; then
    echo "ASSERT FAIL: expected quackapi.http_client=curl in server log" >&2
    grep -E 'http_client|curl_httpfs' "$_QA_LOG" || true
    exit 1
  fi
else
  if ! grep -q 'quackapi.http_client=httplib reason=curl_httpfs_unavailable' "$_QA_LOG"; then
    echo "ASSERT FAIL: expected httplib fallback log line" >&2
    grep -E 'http_client|curl_httpfs' "$_QA_LOG" || true
    exit 1
  fi
fi
echo "   log client line OK"

stop_quackapi

# --- Force httplib even when curl is available ---
INIT2="$(mktemp /tmp/quackapi_curl_force_XXXXXX.sql)"
cat >"$INIT2" <<'SQL'
SELECT 1;
SQL

# boot with custom serve — lib.sh always calls quackapi_serve(port); override by
# feeding serve ourselves after empty init won't work. Use a second manual boot.
_QA_PORT="$PORT_FORCE"
_QA_FIFO="$(mktemp -u /tmp/quackapi_http_XXXXXX.fifo)"
_QA_LOG="$(mktemp /tmp/quackapi_http_XXXXXX.log)"
rm -f "$_QA_FIFO"
mkfifo "$_QA_FIFO"
stale="$(lsof -nP -iTCP:"$PORT_FORCE" -sTCP:LISTEN -t 2>/dev/null || true)"
if [[ -n "$stale" ]]; then
  kill $stale 2>/dev/null || true
  sleep 0.2
fi
"$DUCKDB_BIN" -unsigned <"$_QA_FIFO" >"$_QA_LOG" 2>&1 &
_QA_PID=$!
exec 3>"$_QA_FIFO"
_QA_FD=3
{
  echo "LOAD quackapi;"
  echo "SELECT * FROM quackapi_serve(${PORT_FORCE}, http_client := 'httplib');"
} >&3

for i in $(seq 1 80); do
  if lsof -nP -iTCP:"$PORT_FORCE" -sTCP:LISTEN >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "$_QA_PID" 2>/dev/null; then
    echo "duckdb exited early (force httplib); log:" >&2
    cat "$_QA_LOG" >&2
    exit 3
  fi
  sleep 0.1
done

echo "-- 4. http_client := 'httplib' forces stock client"
curl_json GET "/healthz"
assert_status "$_QA_LAST_STATUS" "200" "healthz forced"
assert_body_contains "$_QA_LAST_BODY" '"http_client":"httplib"' "forced httplib"
if ! grep -q 'quackapi.http_client=httplib reason=operator_forced' "$_QA_LOG"; then
  echo "ASSERT FAIL: expected operator_forced log" >&2
  grep -E 'http_client' "$_QA_LOG" || true
  exit 1
fi
echo "   forced httplib OK"

stop_quackapi
rm -f "$INIT2"

echo "curl_httpfs_client.test.sh OK"
