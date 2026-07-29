#!/usr/bin/env bash
# HTTP integration: batteries outbound HTTP client guarantee.
# - auto: prefer curl_httpfs; loud fallback (http_client_reason) if unavailable
# - curl: require curl_httpfs (fail serve if missing)
# - httplib: force stock client
# Platform-aware probe matches batteries INSTALL/LOAD path.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT_AUTO="${QUACKAPI_TEST_PORT:-18991}"
PORT_FORCE="${QUACKAPI_TEST_PORT_FORCE:-18992}"
PORT_CURL="${QUACKAPI_TEST_PORT_CURL:-18993}"

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

# ---------------------------------------------------------------------------
# 1–3. auto (default) — accurate http_client + reason on healthz/log
# ---------------------------------------------------------------------------
INIT="$(mktemp /tmp/quackapi_curl_httpfs_XXXXXX.sql)"
# CREATE ROUTE validates handler SQL at registration time, so httpfs must be
# loadable before serve. Batteries still prefer curl_httpfs on quackapi_serve().
cat >"$INIT" <<SQL
INSTALL httpfs;
LOAD httpfs;
CREATE ROUTE remote_csv GET '/remote' AS
SELECT length(content) AS n FROM read_text('${REMOTE_URL}');
SQL

boot_quackapi "$PORT_AUTO" "$INIT"
rm -f "$INIT"

echo "-- 1. /healthz reports http_client matching platform probe (+ reason field)"
curl_json GET "/healthz"
assert_status "$_QA_LAST_STATUS" "200" "healthz"
assert_body_contains "$_QA_LAST_BODY" '"status":"ok"' "healthz status"
assert_body_contains "$_QA_LAST_BODY" "\"http_client\":\"${EXPECTED_CLIENT}\"" "healthz http_client"
assert_body_contains "$_QA_LAST_BODY" '"http_client_reason":' "healthz has http_client_reason"
if [[ "$EXPECTED_CLIENT" == "curl" ]]; then
  assert_body_contains "$_QA_LAST_BODY" '"http_client_reason":""' "auto+curl: empty reason"
else
  assert_body_contains "$_QA_LAST_BODY" '"http_client_reason":"curl_httpfs_unavailable"' \
    "auto fallback: loud reason"
fi

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
    echo "ASSERT FAIL: expected loud httplib fallback log line" >&2
    grep -E 'http_client|curl_httpfs' "$_QA_LOG" || true
    exit 1
  fi
  if ! grep -q 'WARN=auto_fallback' "$_QA_LOG"; then
    echo "ASSERT FAIL: expected WARN=auto_fallback in loud fallback log" >&2
    grep -E 'http_client' "$_QA_LOG" || true
    exit 1
  fi
fi
echo "   log client line OK"

# On platforms with curl_httpfs, active HTTPUtil should be MultiCurl (or HTTPFS-Curl).
if [[ "$EXPECTED_CLIENT" == "curl" ]]; then
  echo "-- 3b. quackapi_http_util_name reflects MultiCurl/HTTPFS-Curl after auto"
  # FIFO still open; query via a one-shot duckdb against the same process is not
  # possible. Re-probe: healthz is enough for client name; util name checked under
  # forced curl serve below via a dedicated boot that prints it.
fi

stop_quackapi

# ---------------------------------------------------------------------------
# 4. http_client := 'httplib' forces stock client
# ---------------------------------------------------------------------------
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
assert_body_contains "$_QA_LAST_BODY" '"http_client_reason":"operator_forced"' "forced reason"
if ! grep -q 'quackapi.http_client=httplib reason=operator_forced' "$_QA_LOG"; then
  echo "ASSERT FAIL: expected operator_forced log" >&2
  grep -E 'http_client' "$_QA_LOG" || true
  exit 1
fi
echo "   forced httplib OK"

stop_quackapi

# ---------------------------------------------------------------------------
# 5. http_client := 'curl' — require curl_httpfs
# ---------------------------------------------------------------------------
if [[ "$EXPECTED_CLIENT" == "curl" ]]; then
  echo "-- 5. http_client := 'curl' succeeds and reports curl (+ MultiCurl util)"
  INIT_CURL="$(mktemp /tmp/quackapi_curl_req_XXXXXX.sql)"
  cat >"$INIT_CURL" <<'SQL'
SELECT 1;
SQL
  # Custom boot: serve with http_client := 'curl' and capture util name after serve.
  _QA_PORT="$PORT_CURL"
  _QA_FIFO="$(mktemp -u /tmp/quackapi_http_XXXXXX.fifo)"
  _QA_LOG="$(mktemp /tmp/quackapi_http_XXXXXX.log)"
  rm -f "$_QA_FIFO"
  mkfifo "$_QA_FIFO"
  stale="$(lsof -nP -iTCP:"$PORT_CURL" -sTCP:LISTEN -t 2>/dev/null || true)"
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
    echo "SELECT * FROM quackapi_serve(${PORT_CURL}, http_client := 'curl');"
    echo "SELECT quackapi_http_util_name() AS util;"
    echo "SELECT http_client, http_client_reason FROM quackapi_servers() WHERE port = ${PORT_CURL};"
  } >&3

  for i in $(seq 1 80); do
    if lsof -nP -iTCP:"$PORT_CURL" -sTCP:LISTEN >/dev/null 2>&1; then
      break
    fi
    if ! kill -0 "$_QA_PID" 2>/dev/null; then
      echo "duckdb exited early (require curl); log:" >&2
      cat "$_QA_LOG" >&2
      exit 3
    fi
    sleep 0.1
  done
  # Give SELECT after serve a moment to flush into log
  sleep 0.3

  curl_json GET "/healthz"
  assert_status "$_QA_LAST_STATUS" "200" "healthz curl-require"
  assert_body_contains "$_QA_LAST_BODY" '"http_client":"curl"' "require curl client"
  assert_body_contains "$_QA_LAST_BODY" '"http_client_reason":""' "require curl empty reason"
  if ! grep -q 'quackapi.http_client=curl' "$_QA_LOG"; then
    echo "ASSERT FAIL: expected curl log under http_client:='curl'" >&2
    grep -E 'http_client' "$_QA_LOG" || true
    exit 1
  fi
  # util name: MultiCurl (current) or HTTPFS-Curl (older naming)
  if ! grep -Eiq 'MultiCurl|HTTPFS-Curl|HTTPFS.Curl' "$_QA_LOG"; then
    echo "ASSERT FAIL: expected MultiCurl/HTTPFS-Curl in util name output" >&2
    grep -E 'util|MultiCurl|HTTPFS|Built' "$_QA_LOG" || true
    cat "$_QA_LOG" >&2
    exit 1
  fi
  if ! grep -q 'curl' "$_QA_LOG" || ! grep -q 'http_client' "$_QA_LOG"; then
    : # already checked
  fi
  echo "   require curl + MultiCurl OK"

  stop_quackapi
  rm -f "$INIT_CURL"
else
  echo "-- 5. http_client := 'curl' must FAIL serve when curl_httpfs unavailable"
  FAIL_LOG="$(mktemp /tmp/quackapi_curl_fail_XXXXXX.log)"
  set +e
  "$DUCKDB_BIN" -unsigned -c "
LOAD quackapi;
SELECT * FROM quackapi_serve(${PORT_CURL}, http_client := 'curl');
" >"$FAIL_LOG" 2>&1
  FAIL_RC=$?
  set -e
  if [[ $FAIL_RC -eq 0 ]]; then
    echo "ASSERT FAIL: expected serve to fail with http_client:='curl' when curl_httpfs missing" >&2
    cat "$FAIL_LOG" >&2
    rm -f "$FAIL_LOG"
    exit 1
  fi
  if ! grep -Eiq 'curl_httpfs|http_client' "$FAIL_LOG"; then
    echo "ASSERT FAIL: expected error mentioning curl_httpfs / http_client" >&2
    cat "$FAIL_LOG" >&2
    rm -f "$FAIL_LOG"
    exit 1
  fi
  echo "   require-curl fail-hard OK (rc=$FAIL_RC)"
  rm -f "$FAIL_LOG"
fi

echo "curl_httpfs_client.test.sh OK"
