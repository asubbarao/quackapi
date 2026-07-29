#!/usr/bin/env bash
# Boot stack A (quackapi). Each process is a full DuckDB: routes + libpq pg_dsn.
# WORKERS=1 (default) → one process on PORT.
# WORKERS=N → N processes all bind PORT via SO_REUSEPORT (httplib default) —
#   kernel distributes accepts (same idea as uvicorn multi-worker, no proxy tax).
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-/Users/aloksubbarao/personal/quackapi/build/release/duckdb}"
EXT="${QUACKAPI_EXT:-/Users/aloksubbarao/personal/quackapi/build/release/extension/quackapi/quackapi.duckdb_extension}"
ROUTES="${BENCH_DIR}/routes.sql"
HOST="${QUACKAPI_HOST:-127.0.0.1}"
PORT="${QUACKAPI_PORT:-8000}"
WORKERS="${WORKERS:-1}"

if [[ ! -x "$DUCKDB_BIN" ]]; then
  echo "serve_quackapi: duckdb binary not found: $DUCKDB_BIN" >&2
  exit 1
fi
if [[ ! -f "$EXT" ]]; then
  echo "serve_quackapi: extension not found: $EXT" >&2
  exit 1
fi
if [[ ! -f "$ROUTES" ]]; then
  echo "serve_quackapi: routes file not found: $ROUTES" >&2
  exit 1
fi

boot_one() {
  local port="$1"
  # Cap PG pool per process so WORKERS×pool stays under Postgres max_connections.
  local pool=32
  local duck_threads=4
  local httpt=32
  if [[ "$WORKERS" -gt 1 ]]; then
    pool=4
    duck_threads=1
    httpt=8
  fi
  {
    printf "LOAD '%s';\n" "$EXT"
    sed -e "s/SET pg_pool_max_connections = 64;/SET pg_pool_max_connections = ${pool};\nSET pg_pool_acquire_mode = 'wait';\nSET threads = ${duck_threads};/" \
        "$ROUTES"
    # pg_dsn → libpq path (FastAPI-shaped). Same DSN as ATTACH.
    printf "SELECT * FROM quackapi_serve(%s, host := '%s', access_log := false, enable_logging := false, worker_threads := %s, pg_dsn := 'postgresql://admin:password@127.0.0.1:6432/quackbench');\n" \
      "$port" "$HOST" "$httpt"
    while :; do sleep 86400; done
  } | exec "$DUCKDB_BIN" -unsigned -init /dev/null :memory:
}

if [[ "$WORKERS" -le 1 ]]; then
  boot_one "$PORT"
  exit 0
fi

# Multi-process SO_REUSEPORT: every worker binds PORT (no Python RR proxy).
PIDS=()
cleanup() {
  local p
  for p in "${PIDS[@]:-}"; do
    kill "$p" 2>/dev/null || true
    # children of the bash wrapper that owns the duckdb pipe
    kill -- -"$p" 2>/dev/null || true
  done
}
trap cleanup EXIT INT TERM

i=1
while [[ $i -le $WORKERS ]]; do
  boot_one "$PORT" &
  PIDS+=($!)
  # brief stagger so first bind establishes before peers join REUSEPORT group
  sleep 0.05
  i=$((i + 1))
done

# Wait until PORT answers /hello (any worker).
ready=0
for _ in $(seq 1 120); do
  if curl -sf --max-time 1 "http://127.0.0.1:${PORT}/hello" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.25
done
if [[ $ready -ne 1 ]]; then
  echo "serve_quackapi: SO_REUSEPORT workers not ready on :${PORT} (n=${WORKERS})" >&2
  exit 1
fi

echo "serve_quackapi: ${WORKERS} workers SO_REUSEPORT on :${PORT}" >&2

# Hold until killed (trap cleans children).
wait
