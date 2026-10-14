#!/usr/bin/env bash
# Live FastAPI-equivalence harness for quackapi (versioned under test/conformance/).
# Uses FIFO interactive session (duckdb -c parses all statements upfront — serve would block).
set -euo pipefail

CONF="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$CONF/../.." && pwd)"
DUCK="${DUCKDB:-$REPO/build/release/duckdb}"
EXT="${QUACKAPI_EXT:-$REPO/build/release/extension/quackapi/quackapi.duckdb_extension}"
EXPECTED_EXT_VERSION="${QUACKAPI_EXPECTED_VERSION:-$(git -C "$REPO" rev-parse --short HEAD)}"
PORT="${PORT:-18770}"
BASE="http://127.0.0.1:${PORT}"
FIFO="${TMPDIR:-/tmp}/quackapi_conformance_$$.fifo"
LOG="${TMPDIR:-/tmp}/quackapi_conformance_$$.log"
PIDFILE="${TMPDIR:-/tmp}/quackapi_conformance_$$.pid"
RESULTS_DIR="${RESULTS_DIR:-$CONF/results}"
DPID=""

cleanup() {
  if [[ -n "$DPID" ]] && kill -0 "$DPID" 2>/dev/null; then
    kill "$DPID" 2>/dev/null || true
    wait "$DPID" 2>/dev/null || true
  fi
  rm -f "$FIFO" "$PIDFILE"
}
trap cleanup EXIT

if [[ ! -x "$DUCK" ]]; then
  echo "duckdb binary not found/executable: $DUCK" >&2
  echo "Build first: CMAKE_BUILD_PARALLEL_LEVEL=4 MAKEFLAGS=-j4 make release" >&2
  exit 2
fi

if [[ ! -f "$EXT" ]]; then
  echo "quackapi extension artifact not found: $EXT" >&2
  exit 2
fi
if [[ "$EXT" == *"'"* ]]; then
  echo "quackapi artifact path contains a SQL quote: $EXT" >&2
  exit 2
fi
core_version="$("$DUCK" -unsigned -noheader -list -c 'SELECT library_version, source_id FROM pragma_version();')"
if [[ "$core_version" != 'v1.5.6|069cc9f9' ]]; then
  echo "wrong DuckDB core: $core_version (expected v1.5.6|069cc9f9)" >&2
  exit 2
fi
actual_ext_version="$("$DUCK" -unsigned -noheader -list -c "LOAD '$EXT'; SELECT extension_version FROM duckdb_extensions() WHERE extension_name = 'quackapi' AND loaded;")"
if [[ "$actual_ext_version" != "$EXPECTED_EXT_VERSION" ]]; then
  echo "wrong quackapi artifact: $actual_ext_version (expected $EXPECTED_EXT_VERSION from $EXT)" >&2
  exit 2
fi

if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN -t >/dev/null 2>&1; then
  echo "Port $PORT is already occupied; choose PORT for an isolated run" >&2
  exit 2
fi
mkfifo "$FIFO"

"$DUCK" -unsigned <"$FIFO" >"$LOG" 2>&1 &
echo $! >"$PIDFILE"
DPID=$!

exec 3>"$FIFO"

{
  echo "SET threads = 4;"
  echo "SET memory_limit = '256MB';"
  echo "SET preserve_insertion_order = false;"
  printf "LOAD '%s';\n" "$EXT"
  cat "$CONF/routes.sql"
  echo "SELECT * FROM quackapi_serve(${PORT}, health_routes := false, access_log := false);"
  echo "SELECT * FROM quackapi_servers();"
} >&3

for i in $(seq 1 50); do
  if curl -sS -o /dev/null --connect-timeout 0.2 "$BASE/health" 2>/dev/null; then
    break
  fi
  if ! kill -0 "$DPID" 2>/dev/null; then
    echo "duckdb exited early; log:" >&2
    cat "$LOG" >&2
    exit 3
  fi
  sleep 0.1
done

if ! curl -sS -o /dev/null --connect-timeout 1 "$BASE/health" 2>/dev/null; then
  echo "server did not become ready on $BASE; log:" >&2
  cat "$LOG" >&2
  exit 3
fi

echo "quackapi listening on $BASE (pid $DPID)"

mkdir -p "$RESULTS_DIR"
export QUACKAPI_BASE="$BASE"
DRIVER_RC=0
python3 "$CONF/driver.py" --base "$BASE" --cases "$CONF/cases.jsonl" --out "$RESULTS_DIR/results.jsonl" || DRIVER_RC=$?

echo "SELECT * FROM quackapi_stop();" >&3
exec 3>&-
sleep 0.3
if kill -0 "$DPID" 2>/dev/null; then
  kill "$DPID" 2>/dev/null || true
  wait "$DPID" 2>/dev/null || true
fi
rm -f "$PIDFILE"
DPID=""

echo "driver exit=$DRIVER_RC"
echo "server log=$LOG"
exit "$DRIVER_RC"
