#!/usr/bin/env bash
# Boot FastAPI benchmark stack B on 127.0.0.1:8001 (foreground).
# Serves the four routes against live pgEdge Postgres (no local DB file).
# WORKERS env selects uvicorn process count (default 1; bench runner also runs 8).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

VENV="${ROOT}/.venv"
WORKERS="${WORKERS:-1}"

if [[ ! -x "${VENV}/bin/uvicorn" ]]; then
  echo "error: ${VENV} missing or incomplete — create with uv (see README)" >&2
  exit 1
fi

exec "${VENV}/bin/uvicorn" fastapi_app:app \
  --host 127.0.0.1 \
  --port 8001 \
  --workers "$WORKERS"
