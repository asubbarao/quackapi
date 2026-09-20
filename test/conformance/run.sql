-- Live HTTP contract harness for quackapi.
--
-- Run from the repository root, after `make release`:
--   build/release/duckdb -no-init -f test/conformance/run.sql
--
-- Controls: PORT (18770), DUCKDB (build/release/duckdb),
-- RESULTS_DIR (test/conformance/results), QUACKAPI_TEST_HOME (build/test-home).
--
-- The server runs behind a FIFO because quackapi_serve blocks: `duckdb -c`
-- parses every statement up front, so the stop statement would never be read.
-- The session is fed statement by statement through the FIFO instead, which is
-- also how the run is stopped cleanly rather than by killing the process.
--
-- HOME is redirected at QUACKAPI_TEST_HOME so the session cannot pick up an
-- extension from the developer's own ~/.duckdb.

INSTALL shellfs FROM community;
LOAD shellfs;

CREATE OR REPLACE TABLE conformance_run AS
SELECT * FROM read_csv('bash -c ''
set -u
PORT=${PORT:-18770}
DUCK=${DUCKDB:-build/release/duckdb}
BASE=http://127.0.0.1:$PORT
RESULTS_DIR=${RESULTS_DIR:-test/conformance/results}
TEST_HOME=${QUACKAPI_TEST_HOME:-build/test-home}
FIFO=${TMPDIR:-/tmp}/quackapi_conformance_$$.fifo
LOG=${TMPDIR:-/tmp}/quackapi_conformance_$$.log

if [ ! -x "$DUCK" ]; then
  echo "duckdb binary not found/executable: $DUCK" >&2
  echo "Build first: CMAKE_BUILD_PARALLEL_LEVEL=4 MAKEFLAGS=-j4 make release" >&2
  printf "build|2\n"; exit 0
fi
if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN -t >/dev/null 2>&1; then
  echo "Port $PORT is already occupied; choose PORT for an isolated run" >&2
  printf "port|2\n"; exit 0
fi

mkdir -p "$TEST_HOME" "$RESULTS_DIR"
mkfifo "$FIFO"
HOME=$TEST_HOME USERPROFILE=$TEST_HOME "$DUCK" -no-init -unsigned <"$FIFO" >"$LOG" 2>&1 &
DPID=$!
exec 3>"$FIFO"

printf "INSTALL curl_httpfs FROM community;\n" >&3
printf "INSTALL httpfs_timeout_retry FROM community;\n" >&3
printf "LOAD quackapi;\n" >&3
cat test/conformance/routes.sql >&3
printf "SELECT * FROM quackapi_serve(%s, health_routes := false, access_log := false);\n" "$PORT" >&3
printf "SELECT * FROM quackapi_servers();\n" >&3

ready=0
for _ in $(seq 1 50); do
  curl -sS -o /dev/null --connect-timeout 0.2 "$BASE/health" 2>/dev/null && { ready=1; break; }
  kill -0 "$DPID" 2>/dev/null || break
  sleep 0.1
done
if [ "$ready" -ne 1 ]; then
  echo "server did not become ready on $BASE; log:" >&2
  cat "$LOG" >&2
  exec 3>&-
  kill "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null
  rm -f "$FIFO"
  printf "ready|3\n"; exit 0
fi
echo "quackapi listening on $BASE (pid $DPID)" >&2

QUACKAPI_BASE=$BASE python3 test/conformance/driver.py --base "$BASE" --cases test/conformance/cases.jsonl --out "$RESULTS_DIR/results.jsonl"
printf "driver|%s\n" "$?"

printf "SELECT * FROM quackapi_stop();\n" >&3
exec 3>&-
sleep 0.3
kill "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null
rm -f "$FIFO"
echo "server log=$LOG" >&2
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['gate', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

-- A gate that never reported is itself a failure: the driver has to run.
WITH recorded AS (
  SELECT coalesce(array_agg(gate ORDER BY gate) FILTER (WHERE exit_code <> 0), []) AS failed,
         coalesce(array_agg(gate), []) AS gates
  FROM conformance_run
)
SELECT CASE
  WHEN len(gates) = 0 THEN error('FAIL: the conformance harness reported nothing')
  WHEN len(failed) > 0 THEN error('FAIL: ' || list_aggregate(failed, 'string_agg', ' '))
  ELSE 'PASS: the live HTTP contract suite is green' END AS verdict
FROM recorded;
