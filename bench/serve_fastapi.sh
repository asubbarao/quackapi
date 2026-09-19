#!/usr/bin/env bash
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV="${BENCH_VENV:-${BENCH_DIR}/.venv}"
PORT="${FASTAPI_PORT:-18081}"
: "${UPSTREAM_SOURCE_PARENT:?UPSTREAM_SOURCE_PARENT is required}"
: "${FASTAPI_SURVIVORS:?FASTAPI_SURVIVORS is required}"

exec "${VENV}/bin/uvicorn" fastapi_adapter:app \
  --app-dir "${BENCH_DIR}" \
  --host 127.0.0.1 \
  --port "${PORT}" \
  --workers 1 \
  --no-access-log
