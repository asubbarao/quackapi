#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DUCKDB="${DUCKDB:-${ROOT}/build/release/duckdb}"
EXT="${QUACKAPI_EXT:-${ROOT}/build/release/extension/quackapi/quackapi.duckdb_extension}"
PORT="${PORT:-18772}"
HOST="${HOST:-127.0.0.1}"
FIFO="${TMPDIR:-/tmp}/quackapi_ultra_${PORT}_$$.fifo"
LOG="${TMPDIR:-/tmp}/quackapi_ultra_${PORT}_$$.log"

if [[ ! -x "$DUCKDB" ]]; then
  echo "missing DuckDB binary: $DUCKDB" >&2
  exit 2
fi
if [[ ! -f "$EXT" ]]; then
  echo "missing quackapi extension: $EXT" >&2
  exit 2
fi

cleanup() {
  exec 3>&- 2>/dev/null || true
  if [[ -n "${DPID:-}" ]] && kill -0 "$DPID" 2>/dev/null; then
    kill "$DPID" 2>/dev/null || true
    wait "$DPID" 2>/dev/null || true
  fi
  rm -f "$FIFO"
}
trap cleanup EXIT INT TERM

mkfifo "$FIFO"
"$DUCKDB" -init /dev/null -unsigned <"$FIFO" >"$LOG" 2>&1 &
DPID=$!
exec 3>"$FIFO"

printf "LOAD '%s';\n" "${EXT//\'/\'\'}" >&3
cat "${ROOT}/test/ultra/routes.sql" >&3
printf "SELECT * FROM quackapi_serve(%s, host := '%s', health_routes := false, access_log := false, enable_logging := false, compression := true, cors_origins := 'http://example.test');\n" "$PORT" "$HOST" >&3

ready=0
for _ in $(seq 1 120); do
  if curl -sf --max-time 1 "http://${HOST}:${PORT}/matrix/health" >/dev/null 2>&1; then
    ready=1
    break
  fi
  if ! kill -0 "$DPID" 2>/dev/null; then
    cat "$LOG" >&2
    exit 3
  fi
  sleep 0.1
done
if [[ "$ready" -ne 1 ]]; then
  echo "QuackAPI did not become ready on ${HOST}:${PORT}" >&2
  cat "$LOG" >&2
  exit 3
fi

echo "quackapi ultra server ready on http://${HOST}:${PORT} (pid=${DPID})"
wait "$DPID"

