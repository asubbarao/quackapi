#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ULTRA="${ROOT}/test/ultra"
QUACK_PORT="${QUACK_PORT:-18772}"
FASTAPI_PORT="${FASTAPI_PORT:-18773}"
RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
RESULTS="${RESULTS_DIR:-${ULTRA}/results/${RUN_ID}}"
VENV="${FASTAPI_VENV:-${ROOT}/bench/.venv}"
DUCKDB="${DUCKDB:-${ROOT}/build/release/duckdb}"
EXT="${QUACKAPI_EXT:-${ROOT}/build/release/extension/quackapi/quackapi.duckdb_extension}"

mkdir -p "$RESULTS"
QP=""
FP=""
cleanup() {
  if [[ -n "$FP" ]] && kill -0 "$FP" 2>/dev/null; then kill "$FP" 2>/dev/null || true; fi
  if [[ -n "$QP" ]] && kill -0 "$QP" 2>/dev/null; then kill "$QP" 2>/dev/null || true; fi
  wait "$FP" 2>/dev/null || true
  wait "$QP" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

PORT="$QUACK_PORT" DUCKDB="$DUCKDB" QUACKAPI_EXT="$EXT" \
  bash "${ULTRA}/serve_quack.sh" >"${RESULTS}/quackapi.log" 2>&1 &
QP=$!

if [[ ! -x "${VENV}/bin/uvicorn" ]]; then
  echo "missing FastAPI venv: ${VENV}" >&2
  exit 2
fi
(
  cd "$ULTRA"
  PYTHONPATH="$ULTRA" "${VENV}/bin/uvicorn" fastapi_app:app --host 127.0.0.1 --port "$FASTAPI_PORT" --workers 1
) >"${RESULTS}/fastapi.log" 2>&1 &
FP=$!

for _ in $(seq 1 120); do
  q=0; f=0
  curl -sf --max-time 1 "http://127.0.0.1:${QUACK_PORT}/matrix/health" >/dev/null 2>&1 && q=1 || true
  curl -sf --max-time 1 "http://127.0.0.1:${FASTAPI_PORT}/matrix/health" >/dev/null 2>&1 && f=1 || true
  if [[ "$q" -eq 1 && "$f" -eq 1 ]]; then break; fi
  if ! kill -0 "$QP" 2>/dev/null || ! kill -0 "$FP" 2>/dev/null; then
    cat "${RESULTS}/quackapi.log" >&2 || true
    cat "${RESULTS}/fastapi.log" >&2 || true
    exit 3
  fi
  sleep 0.1
done

driver_args=(
  --quack "http://127.0.0.1:${QUACK_PORT}"
  --fastapi "http://127.0.0.1:${FASTAPI_PORT}"
  --out "$RESULTS"
)
if [[ -n "${NO_FUZZ:-}" ]]; then
  driver_args+=(--no-fuzz)
fi
python3 "${ULTRA}/paired_driver.py" "${driver_args[@]}"

echo "paired matrix results: ${RESULTS}"

