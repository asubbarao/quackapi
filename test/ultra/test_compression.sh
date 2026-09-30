#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PORT="${COMPRESSION_PORT:-18773}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/quackapi-compression.XXXXXX")"
SERVER_LOG="${WORK}/server.log"
mkdir -p "${WORK}/home"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

HOME="${WORK}/home" PORT="$PORT" bash "${ROOT}/test/ultra/serve_quack.sh" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 120); do
  if curl -sf --max-time 1 "http://127.0.0.1:${PORT}/matrix/health" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    sed -n '1,220p' "$SERVER_LOG"
    exit 1
  fi
  sleep 0.1
done

header() {
  awk -v wanted="$1" 'BEGIN {IGNORECASE=1} tolower($1) == tolower(wanted ":") {gsub(/\r/, "", $2); print $2}' "$2" | tail -1
}

check_response() {
  local name="$1" accept="$2" expected_encoding="$3" decoder="$4"
  local headers="${WORK}/${name}.headers" body="${WORK}/${name}.body" decoded="${WORK}/${name}.decoded"
  curl -sS --http1.1 -H "Accept-Encoding: ${accept}" -D "$headers" -o "$body" \
    "http://127.0.0.1:${PORT}/matrix/compressed"
  local encoding vary length bytes
  encoding="$(header Content-Encoding "$headers")"
  vary="$(header Vary "$headers")"
  length="$(header Content-Length "$headers")"
  bytes="$(wc -c <"$body" | tr -d ' ')"
  [[ "${encoding:-}" == "$expected_encoding" ]] || { echo "${name}: encoding=${encoding:-identity}"; return 1; }
  [[ "$vary" == "Accept-Encoding" ]] || { echo "${name}: vary=${vary:-missing}"; return 1; }
  [[ "$length" == "$bytes" ]] || { echo "${name}: length=${length} bytes=${bytes}"; return 1; }
  case "$decoder" in
    gzip) gzip -dc "$body" >"$decoded" ;;
    zstd) command -v zstd >/dev/null || { echo "zstd CLI is required for this curl check"; return 2; }; zstd -q -dc "$body" >"$decoded" ;;
    identity) cp "$body" "$decoded" ;;
  esac
  grep -q '"payload":"xxxxxxxx' "$decoded"
}

check_response gzip gzip gzip gzip
check_response zstd zstd zstd zstd
check_response both 'gzip, zstd' zstd zstd
check_response q_gzip 'gzip;q=0.8, zstd;q=0.2' gzip gzip
check_response q_zstd 'gzip;q=0.2, zstd;q=0.8' zstd zstd
check_response neither identity '' identity

small_headers="${WORK}/small.headers"
small_body="${WORK}/small.body"
curl -sS --http1.1 -H 'Accept-Encoding: gzip, zstd' -D "$small_headers" -o "$small_body" \
  "http://127.0.0.1:${PORT}/matrix/small"
[[ -z "$(header Content-Encoding "$small_headers")" ]]
[[ -z "$(header Vary "$small_headers")" ]]

sse_headers="${WORK}/sse.headers"
sse_body="${WORK}/sse.body"
curl -sS --http1.1 -H 'Accept-Encoding: gzip, zstd' -D "$sse_headers" -o "$sse_body" \
  "http://127.0.0.1:${PORT}/matrix/stream"
[[ "$(header Content-Type "$sse_headers")" == text/event-stream ]]
[[ -z "$(header Content-Encoding "$sse_headers")" ]]
grep -q '^data:' "$sse_body"

echo "compression curl checks passed"
