#!/usr/bin/env bash
# Boot FastAPI benchmark stack B on 127.0.0.1:8001 (foreground).
# Serves the four routes against live pgEdge Postgres (no local DB file).
# WORKERS env selects uvicorn process count (default 1; bench runner also runs 8).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

VENV="${ROOT}/.venv"
WORKERS="${WORKERS:-1}"

if [[ "$WORKERS" -gt 1 ]]; then
  export BENCH_WORKER_THREADS="${BENCH_WORKER_THREADS:-4}"
  export BENCH_PG_POOL_MAX="${BENCH_PG_POOL_MAX:-4}"
else
  export BENCH_WORKER_THREADS="${BENCH_WORKER_THREADS:-32}"
  export BENCH_PG_POOL_MAX="${BENCH_PG_POOL_MAX:-32}"
fi

if [[ ! -x "${VENV}/bin/uvicorn" ]]; then
  echo "error: ${VENV} missing or incomplete — create with uv (see README)" >&2
  exit 1
fi

echo "serve_fastapi: effective_settings workers=${WORKERS} http_threads=${BENCH_WORKER_THREADS} pg_pool=${BENCH_PG_POOL_MAX}" >&2

"${VENV}/bin/uvicorn" fastapi_app:app \
  --host 127.0.0.1 \
  --port 8001 \
  --workers "$WORKERS" &
UVICORN_PID=$!
cleanup() {
  kill "$UVICORN_PID" 2>/dev/null || true
  wait "$UVICORN_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Uvicorn's parent owns a short-lived spawn helper as well as one child per
# worker on some versions. Emit only the actual application worker PIDs so the
# runner can verify the requested worker count without mistaking the helper for
# a server process.
worker_pids=""
for _ in $(seq 1 120); do
  if [[ "$WORKERS" -eq 1 ]]; then
    worker_pids="$UVICORN_PID"
    break
  fi
  worker_pids=""
  for pid in $(pgrep -P "$UVICORN_PID" 2>/dev/null || true); do
    command_line="$(ps -p "$pid" -o command= 2>/dev/null || true)"
    if [[ "$command_line" == *"fastapi_app:app"* || "$command_line" == *"multiprocessing-fork"* ]]; then
      worker_pids+="${pid} "
    fi
  done
  worker_pids="${worker_pids% }"
  # Older Uvicorn releases do not expose the application target in child
  # command lines. Their child list still contains the requested workers plus
  # one spawn helper, so retain that authoritative live set as a fallback.
  if [[ -z "$worker_pids" ]]; then
    worker_pids="$(pgrep -P "$UVICORN_PID" 2>/dev/null | tr '\n' ' ' | sed 's/[[:space:]]*$//' || true)"
  fi
  count=0
  [[ -n "$worker_pids" ]] && count=$(wc -w <<<"$worker_pids" | tr -d ' ')
  if [[ "$count" -ge "$WORKERS" ]]; then
    break
  fi
  sleep 0.25
done
if [[ -z "$worker_pids" ]]; then
  echo "serve_fastapi: worker startup failed (requested=${WORKERS})" >&2
  exit 1
fi
echo "serve_fastapi: worker_pids=${worker_pids}" >&2
wait "$UVICORN_PID"
