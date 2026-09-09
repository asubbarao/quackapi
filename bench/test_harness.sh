#!/usr/bin/env bash
# Isolated runner checks: no HTTP listeners or database connections are created.
set -euo pipefail
TEST_BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
export K6=/usr/bin/true DUCKDB_BIN=/usr/bin/true PSQL=/usr/bin/true
export PORT_FREE_TIMEOUT_SEC=2
source "${TEST_BENCH_DIR}/run.sh"

(
  PG_DSN="host=isolated.invalid dbname=bench"
  psql_stub() { [[ "$1" == "$PG_DSN" ]]; }
  PSQL=psql_stub
  psql_q 'SELECT 1'
  truncate_bench_writes
)

# Inspect the actual startup emitter without loading an extension or attaching PG.
for worker_count in 1 8; do
  generated="$(
    export WORKERS="$worker_count" QUACKAPI_EXT="${TEST_BENCH_DIR}/routes.sql"
    export PG_DSN="host=isolated.invalid dbname=bench password='synthetic'"
    source "${TEST_BENCH_DIR}/serve_quackapi.sh"
    emit_sql 8000
  )"
  expected_pool=32 expected_threads=4 expected_http=32
  if [[ "$worker_count" == 8 ]]; then
    expected_pool=4 expected_threads=1 expected_http=4
  fi
  [[ "$generated" == *"SET pg_pool_max_connections = ${expected_pool};"* ]]
  [[ "$generated" == *"SET pg_pool_acquire_mode = 'wait';"* ]]
  [[ "$generated" == *"SET threads = ${expected_threads};"* ]]
  [[ "$generated" == *"worker_threads := ${expected_http}"* ]]
  [[ "$generated" == *"ATTACH 'host=isolated.invalid dbname=bench password=''synthetic''' AS pg"* ]]
  [[ "$generated" == *"pg_dsn := 'host=isolated.invalid dbname=bench password=''synthetic'''"* ]]
done

# The real start helper must refuse an occupied port without signaling anything.
(
  SERVER_PID=""
  port_listening() { return 0; }
  kill() { echo "FAIL: attempted to signal an occupied port" >&2; exit 99; }
  if start_server quackapi-w1; then
    echo "FAIL: accepted an occupied port" >&2
    exit 1
  fi
  [[ -z "$SERVER_PID" ]]
)

# Verify actual group cleanup, including a descendant, preserves a separate group.
(
  port_listening() { return 1; }
  set -m
  bash -c 'sleep 60 & wait' &
  SERVER_PID=$!
  SERVER_PORT=8000
  sleep 60 &
  unrelated=$!
  set +m
  sleep 0.1
  owned="$SERVER_PID"
  trap 'kill -KILL -- "-${owned}" 2>/dev/null || true; kill "$unrelated" 2>/dev/null || true; wait "$unrelated" 2>/dev/null || true' EXIT
  stop_server 8000
  if kill -0 -- "-${owned}" 2>/dev/null; then
    echo "FAIL: owned group survived cleanup" >&2
    exit 1
  fi
  kill -0 "$unrelated"
  [[ -z "$SERVER_PID" && -z "$SERVER_PORT" ]]
)

echo "PASS: emitted worker settings and shared escaped DSN; occupied ports refused; owned process group cleaned; unrelated process preserved"
