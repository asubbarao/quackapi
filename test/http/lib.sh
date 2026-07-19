#!/usr/bin/env bash
# Shared helpers for quackapi HTTP integration tests.
# Boot via FIFO/interactive stdin — never duckdb -c for DDL after LOAD.
set -euo pipefail

HTTP_TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HTTP_TEST_DIR/../.." && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-$REPO_ROOT/build/release/duckdb}"

# Per-test state (set by boot_quackapi)
_QA_FIFO=""
_QA_LOG=""
_QA_PID=""
_QA_PORT=""
_QA_FD=""

assert_status() {
  local got="$1" want="$2" label="${3:-status}"
  if [[ "$got" != "$want" ]]; then
    echo "ASSERT FAIL ($label): status got=$got want=$want" >&2
    echo "  body: ${_QA_LAST_BODY:-}" >&2
    return 1
  fi
}

assert_body_contains() {
  local body="$1" needle="$2" label="${3:-body}"
  if [[ "$body" != *"$needle"* ]]; then
    echo "ASSERT FAIL ($label): body missing '$needle'" >&2
    echo "  body: $body" >&2
    return 1
  fi
}

assert_body_not_contains() {
  local body="$1" needle="$2" label="${3:-body}"
  if [[ "$body" == *"$needle"* ]]; then
    echo "ASSERT FAIL ($label): body unexpectedly contains '$needle'" >&2
    echo "  body: $body" >&2
    return 1
  fi
}

# curl_json METHOD path [curl args...]
# Sets: _QA_LAST_STATUS, _QA_LAST_BODY, _QA_LAST_HEADERS
curl_json() {
  local method="$1" path="$2"
  shift 2
  local url="http://127.0.0.1:${_QA_PORT}${path}"
  local tmp
  tmp="$(mktemp)"
  local hdr
  hdr="$(mktemp)"
  set +e
  # HEAD: servers often advertise Content-Length of the would-be body while
  # sending no entity. Plain curl -X HEAD then fails with (18). Use --head
  # + --ignore-content-length; entity body is always empty for HEAD.
  local err
  if [[ "$method" == "HEAD" ]]; then
    err="$(curl -sS --head --ignore-content-length -D "$hdr" -o /dev/null "$@" "$url" 2>&1)"
  else
    err="$(curl -sS -D "$hdr" -o "$tmp" -X "$method" "$@" "$url" 2>&1)"
  fi
  local rc=$?
  set -e
  if [[ $rc -ne 0 ]]; then
    _QA_LAST_STATUS="0"
    _QA_LAST_HEADERS=""
    _QA_LAST_BODY="curl failed rc=$rc: ${err}"
    rm -f "$tmp" "$hdr"
    return 0
  fi
  if [[ "$method" == "HEAD" ]]; then
    _QA_LAST_BODY=""
  else
    _QA_LAST_BODY="$(cat "$tmp")"
  fi
  _QA_LAST_HEADERS="$(cat "$hdr")"
  _QA_LAST_STATUS="$(awk 'NR==1 {print $2}' "$hdr")"
  rm -f "$tmp" "$hdr"
}

# boot_quackapi <port> <init.sql-path-or-heredoc-file>
# Loads extension, runs init SQL statements one-by-one via FIFO, serves on port.
boot_quackapi() {
  local port="$1"
  local init_sql="$2"
  _QA_PORT="$port"
  _QA_FIFO="$(mktemp -u /tmp/quackapi_http_XXXXXX.fifo)"
  _QA_LOG="$(mktemp /tmp/quackapi_http_XXXXXX.log)"
  rm -f "$_QA_FIFO"
  mkfifo "$_QA_FIFO"

  # Reap anything already on the port
  local stale
  stale="$(lsof -nP -iTCP:"$port" -sTCP:LISTEN -t 2>/dev/null || true)"
  if [[ -n "$stale" ]]; then
    kill $stale 2>/dev/null || true
    sleep 0.2
  fi

  if [[ ! -x "$DUCKDB_BIN" ]]; then
    echo "duckdb binary not found: $DUCKDB_BIN" >&2
    return 2
  fi

  "$DUCKDB_BIN" -unsigned <"$_QA_FIFO" >"$_QA_LOG" 2>&1 &
  _QA_PID=$!

  # Keep FIFO writer open on FD 3 for this shell
  exec 3>"$_QA_FIFO"
  _QA_FD=3

  {
    echo "LOAD quackapi;"
    # init SQL file: feed line-by-line groups split on semicolons is fragile;
    # feed whole file (interactive parser accepts multi-statement when not -c).
    cat "$init_sql"
    echo
    echo "SELECT * FROM quackapi_serve(${port});"
  } >&3

  # Wait for listen
  local i
  for i in $(seq 1 80); do
    if curl -sS -o /dev/null --connect-timeout 0.2 "http://127.0.0.1:${port}/" 2>/dev/null; then
      break
    fi
    # also try any route — connection refused until bind
    if curl -sS -o /dev/null --connect-timeout 0.2 "http://127.0.0.1:${port}/health" 2>/dev/null; then
      break
    fi
    if ! kill -0 "$_QA_PID" 2>/dev/null; then
      echo "duckdb exited early during boot; log:" >&2
      cat "$_QA_LOG" >&2
      return 3
    fi
    # Port open check
    if lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then
      # listening but no /health — still ok if routes differ
      sleep 0.05
      break
    fi
    sleep 0.1
  done

  if ! lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then
    echo "server did not listen on port $port; log:" >&2
    cat "$_QA_LOG" >&2
    return 3
  fi
}

stop_quackapi() {
  if [[ -n "${_QA_FD:-}" ]] && [[ -n "${_QA_PID:-}" ]]; then
    if kill -0 "$_QA_PID" 2>/dev/null; then
      echo "SELECT * FROM quackapi_stop();" >&3 2>/dev/null || true
      echo ".quit" >&3 2>/dev/null || true
    fi
  fi
  # Close FIFO writer
  exec 3>&- 2>/dev/null || true
  if [[ -n "${_QA_PID:-}" ]]; then
    local i
    for i in $(seq 1 30); do
      if ! kill -0 "$_QA_PID" 2>/dev/null; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "$_QA_PID" 2>/dev/null; then
      kill "$_QA_PID" 2>/dev/null || true
      wait "$_QA_PID" 2>/dev/null || true
    else
      wait "$_QA_PID" 2>/dev/null || true
    fi
  fi
  if [[ -n "${_QA_PORT:-}" ]]; then
    local stale
    stale="$(lsof -nP -iTCP:"$_QA_PORT" -sTCP:LISTEN -t 2>/dev/null || true)"
    if [[ -n "$stale" ]]; then
      kill $stale 2>/dev/null || true
    fi
  fi
  rm -f "${_QA_FIFO:-}" "${_QA_LOG:-}"
  _QA_PID=""
  _QA_PORT=""
  _QA_FIFO=""
  _QA_LOG=""
  _QA_FD=""
}

# Ensure stop on exit for any test that sourced lib and booted
_qa_cleanup_on_exit() {
  stop_quackapi 2>/dev/null || true
}
trap _qa_cleanup_on_exit EXIT
