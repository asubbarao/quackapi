#!/usr/bin/env bash
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"

bash -n "${BENCH_DIR}/run.sh" "${BENCH_DIR}/serve_fastapi.sh" "${BENCH_DIR}/serve_quackapi.sh"
python3 -m py_compile \
  "${BENCH_DIR}/fetch_upstream.py" "${BENCH_DIR}/migrate.py" \
  "${BENCH_DIR}/fastapi_adapter.py" "${BENCH_DIR}/loadgen.py" \
  "${BENCH_DIR}/conformance.py" "${BENCH_DIR}/crash_client.py"

if rg -n -i 'pgedge|:6432|podman start|/opt/homebrew/bin/k6' \
  "${BENCH_DIR}/run.sh" "${BENCH_DIR}/serve_fastapi.sh" \
  "${BENCH_DIR}/serve_quackapi.sh" "${BENCH_DIR}/report.sql"; then
  echo "FAIL: active harness contains a legacy external dependency" >&2
  exit 1
fi

echo "PASS: active scripts parse and contain no pgEdge/k6 dependency"
