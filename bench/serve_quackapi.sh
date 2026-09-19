#!/usr/bin/env bash
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-${BENCH_DIR}/../build/release/duckdb}"
ROUTES_SQL="${GENERATED_ROUTES_SQL:?GENERATED_ROUTES_SQL is required}"
DATABASE="${QUACKAPI_DATABASE:?QUACKAPI_DATABASE is required}"
PORT="${QUACKAPI_PORT:-18080}"
# The sweep must be able to exceed, or stay under, the server's capacity on
# purpose. Hardcoding these meant the 320-connection cell drove 320 clients at
# a server that can hold 32 + 256 = 288, measuring a configuration choice and
# reporting it as a product limit.
WORKER_THREADS="${QUACKAPI_WORKER_THREADS:-32}"
MAX_PENDING="${QUACKAPI_MAX_PENDING_REQUESTS:-256}"

exec "${DUCKDB_BIN}" -no-init -unsigned "${DATABASE}" \
  -f "${ROUTES_SQL}" \
  -c "SELECT * FROM quackapi_serve(${PORT}, host := '127.0.0.1', access_log := false, enable_logging := false, worker_threads := ${WORKER_THREADS}, max_pending_requests := ${MAX_PENDING}, block := true);"
