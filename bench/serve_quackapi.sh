#!/usr/bin/env bash
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-${BENCH_DIR}/../build/release/duckdb}"
ROUTES_SQL="${GENERATED_ROUTES_SQL:?GENERATED_ROUTES_SQL is required}"
DATABASE="${QUACKAPI_DATABASE:?QUACKAPI_DATABASE is required}"
PORT="${QUACKAPI_PORT:-18080}"

exec "${DUCKDB_BIN}" -no-init -unsigned "${DATABASE}" \
  -f "${ROUTES_SQL}" \
  -c "SELECT * FROM quackapi_serve(${PORT}, host := '127.0.0.1', access_log := false, enable_logging := false, worker_threads := 32, max_pending_requests := 256, block := true);"
