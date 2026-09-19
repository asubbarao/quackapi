#!/usr/bin/env bash
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-${BENCH_DIR}/../build/release/duckdb}"
ROUTES_SQL="${GENERATED_ROUTES_SQL:?GENERATED_ROUTES_SQL is required}"
DATABASE="${QUACKAPI_DATABASE:?QUACKAPI_DATABASE is required}"
PORT="${QUACKAPI_PORT:-18080}"
# The sweep must be able to exceed, or stay under, the server's capacity on
# purpose. Hardcoding this meant the 320-connection cell drove 320 clients at a
# server that held 32 + 256, measuring a configuration choice and reporting it
# as a product limit. worker_threads is the only dial the sweep needs now: the
# pending queue follows it unless QUACKAPI_MAX_PENDING_REQUESTS pins one, which
# is what a cell that wants the two budgets apart deliberately does.
WORKER_THREADS="${QUACKAPI_WORKER_THREADS:-32}"
BUDGET="worker_threads := ${WORKER_THREADS}"
if [[ -n "${QUACKAPI_MAX_PENDING_REQUESTS:-}" ]]; then
  BUDGET="${BUDGET}, max_pending_requests := ${QUACKAPI_MAX_PENDING_REQUESTS}"
fi
# Empty (default) keeps the DuckDB handler path; a DSN selects native libpq.
PG_DSN_ARG=""
if [[ -n "${QUACKAPI_PG_DSN:-}" ]]; then
  PG_DSN_ARG=", pg_dsn := '${QUACKAPI_PG_DSN}'"
fi

# quackapi_serve LOADs curl_httpfs and httpfs_timeout_retry and refuses by name
# without them. Installing here keeps the download out of the measured process.
"${DUCKDB_BIN}" -no-init -unsigned "${DATABASE}" \
  -c "INSTALL curl_httpfs FROM community; INSTALL httpfs_timeout_retry FROM community;"

exec "${DUCKDB_BIN}" -no-init -unsigned "${DATABASE}" \
  -f "${ROUTES_SQL}" \
  -c "SELECT * FROM quackapi_serve(${PORT}, host := '127.0.0.1', access_log := false, enable_logging := false, ${BUDGET}${PG_DSN_ARG}, block := true);"
