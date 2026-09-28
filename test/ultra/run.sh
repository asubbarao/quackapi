#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ULTRA="${ROOT}/test/ultra"

echo "==> ultra unit checks"
python3 -m unittest discover -s "$ULTRA" -p 'test_*.py'

echo "==> paired FastAPI/QuackAPI matrix"
bash "${ULTRA}/run_pair.sh"

echo "==> version-aware DuckDB extension probes"
bash "${ULTRA}/run_extensions.sh"

if [[ "${FULL:-0}" == "1" ]]; then
  echo "==> full HTTP conformance corpus"
  # render_scorecard.py reads the canonical conformance results directory;
  # callers can still override it explicitly when they only need raw output.
  PORT="${CONFORMANCE_PORT:-18774}" RESULTS_DIR="${CONFORMANCE_RESULTS_DIR:-${ROOT}/test/conformance/results}" \
    bash "${ROOT}/test/conformance/run.sh"
  python3 "${ROOT}/test/conformance/render_scorecard.py"
  echo "==> full SQL/C++/benchmark gates are run by the repository CI and can be
repeated with the commands in docs/FASTAPI_ULTRA_MATRIX.md"
fi
