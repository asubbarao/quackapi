#!/usr/bin/env bash
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${BENCH_DIR}/.." && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-${REPO_ROOT}/build/release/duckdb}"
QUACKAPI_EXT="${QUACKAPI_EXT:-${REPO_ROOT}/build/release/extension/quackapi/quackapi.duckdb_extension}"
BENCH_VENV="${BENCH_VENV:-${BENCH_DIR}/.venv}"
WORK_ROOT="${BENCH_WORK_ROOT:-${BENCH_DIR}/.tmp/honest}"
RESULTS_ROOT="${RESULTS_ROOT:-${BENCH_DIR}/results}"
TRIALS="${TRIALS:-3}"
DURATION_SEC="${DURATION_SEC:-3}"
WARMUP_SEC="${WARMUP_SEC:-0.5}"
CONCURRENCY_LEVELS="${CONCURRENCY_LEVELS:-1 8 32 64 320}"
REQUEST_TIMEOUT_SEC="${REQUEST_TIMEOUT_SEC:-12}"
CRASH_JOBS="${CRASH_JOBS:-64}"
CRASH_DELAY_MS="${CRASH_DELAY_MS:-3000}"
QUACKAPI_PORT="${QUACKAPI_PORT:-18080}"
FASTAPI_PORT="${FASTAPI_PORT:-18081}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
RESULTS="${RESULTS_ROOT}/${RUN_ID}"
UPSTREAM_CHECKOUT="${WORK_ROOT}/upstream"
UPSTREAM_APP="${UPSTREAM_CHECKOUT}/docs_src/bigger_applications/app_an_py310"
SERVER_PID=""

export DUCKDB_BIN QUACKAPI_EXT BENCH_VENV QUACKAPI_PORT FASTAPI_PORT

stop_server() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  SERVER_PID=""
}

trap stop_server EXIT INT TERM

require_file() {
  if [[ ! -f "$1" ]]; then
    echo "error: required file not found: $1" >&2
    exit 1
  fi
}

wait_ready() {
  local port="$1" pid="$2" attempt
  for attempt in $(seq 1 160); do
    if ! kill -0 "${pid}" 2>/dev/null; then
      return 1
    fi
    if python3 - "${port}" <<'PY' >/dev/null 2>&1
import sys, urllib.request
urllib.request.urlopen(f"http://127.0.0.1:{sys.argv[1]}/users/rick?token=jessica", timeout=0.5).read()
PY
    then
      return 0
    fi
    sleep 0.125
  done
  return 1
}

prepare() {
  require_file "${DUCKDB_BIN}"
  require_file "${QUACKAPI_EXT}"
  mkdir -p "${WORK_ROOT}" "${RESULTS}" "${RESULTS}/raw"
  python3 "${BENCH_DIR}/fetch_upstream.py" --manifest "${BENCH_DIR}/upstream_files.json" --destination "${UPSTREAM_CHECKOUT}" >/dev/null
  if [[ ! -x "${BENCH_VENV}/bin/uvicorn" ]]; then
    python3 -m venv "${BENCH_VENV}"
  fi
  "${BENCH_VENV}/bin/python" -m pip install --disable-pip-version-check -r "${BENCH_DIR}/requirements.txt"
  "${BENCH_VENV}/bin/python" "${BENCH_DIR}/migrate.py" \
    --duckdb "${DUCKDB_BIN}" --extension "${QUACKAPI_EXT}" \
    --source "${UPSTREAM_APP}" --output-dir "${RESULTS}"
}

record_environment() {
  {
    printf 'run_id=%s\n' "${RUN_ID}"
    printf 'git_head=%s\n' "$(git -C "${REPO_ROOT}" rev-parse HEAD)"
    printf 'upstream_repo=https://github.com/fastapi/fastapi\n'
    printf 'upstream_commit=50113da16fec53b66b80d75e80a89296de4fa5a5\n'
    printf 'duckdb=%s\n' "$("${DUCKDB_BIN}" -no-init -version)"
    printf 'python=%s\n' "$(python3 --version)"
    printf 'worker_threads=%s\nmax_pending_requests=%s\n' "${QUACKAPI_WORKER_THREADS:-32}" "${QUACKAPI_MAX_PENDING_REQUESTS:-256}"
    printf 'trials=%s\nduration_sec=%s\nwarmup_sec=%s\n' "${TRIALS}" "${DURATION_SEC}" "${WARMUP_SEC}"
    printf 'concurrency_levels=%s\n' "${CONCURRENCY_LEVELS}"
    "${BENCH_VENV}/bin/python" -m pip freeze
  } >"${RESULTS}/environment.txt"
}

start_server() {
  local stack="$1" purpose="$2" port
  stop_server
  if [[ "${stack}" == "quackapi" ]]; then
    export GENERATED_ROUTES_SQL="${RESULTS}/generated_routes.sql"
    export QUACKAPI_DATABASE="${RESULTS}/quackapi_${purpose}.duckdb"
    "${BENCH_DIR}/serve_quackapi.sh" >"${RESULTS}/${stack}_${purpose}_server.log" 2>&1 &
    port="${QUACKAPI_PORT}"
  else
    export UPSTREAM_SOURCE_PARENT="$(dirname "${UPSTREAM_APP}")"
    export FASTAPI_SURVIVORS="${RESULTS}/fastapi_survivors.txt"
    "${BENCH_DIR}/serve_fastapi.sh" >"${RESULTS}/${stack}_${purpose}_server.log" 2>&1 &
    port="${FASTAPI_PORT}"
  fi
  SERVER_PID=$!
  if ! wait_ready "${port}" "${SERVER_PID}"; then
    echo "error: ${stack} failed to become ready" >&2
    tail -n 100 "${RESULTS}/${stack}_${purpose}_server.log" >&2 || true
    exit 1
  fi
}

run_stack_trial() {
  local stack="$1" trial="$2" port="${QUACKAPI_PORT}" concurrency raw
  [[ "${stack}" == "fastapi" ]] && port="${FASTAPI_PORT}"
  start_server "${stack}" "trial${trial}"
  if [[ "${trial}" == "1" ]]; then
    "${BENCH_VENV}/bin/python" "${BENCH_DIR}/conformance.py" --stack "${stack}" --port "${port}" >>"${RESULTS}/conformance.jsonl"
  fi
  for concurrency in ${CONCURRENCY_LEVELS}; do
    raw="${RESULTS}/raw/${stack}__trial${trial}__c${concurrency}.csv.gz"
    "${BENCH_VENV}/bin/python" "${BENCH_DIR}/loadgen.py" \
      --url "http://127.0.0.1:${port}/users/rick?token=jessica" \
      --stack "${stack}" --trial "${trial}" --concurrency "${concurrency}" \
      --duration "${DURATION_SEC}" --warmup "${WARMUP_SEC}" --timeout "${REQUEST_TIMEOUT_SEC}" \
      --expect-json '{"username":"rick"}' --raw "${raw}" >>"${RESULTS}/measurements.jsonl"
  done
  stop_server
}

run_crash_case() {
  local stack="$1" port="${QUACKAPI_PORT}" client_json acknowledged survived
  [[ "${stack}" == "fastapi" ]] && port="${FASTAPI_PORT}"
  start_server "${stack}" crash
  client_json="$("${BENCH_VENV}/bin/python" "${BENCH_DIR}/crash_client.py" --port "${port}" --jobs "${CRASH_JOBS}" --delay-ms "${CRASH_DELAY_MS}")"
  acknowledged="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["acknowledged"])' <<<"${client_json}")"
  kill -KILL "${SERVER_PID}"
  wait "${SERVER_PID}" 2>/dev/null || true
  SERVER_PID=""
  if [[ "${stack}" == "quackapi" ]]; then
    survived="$("${DUCKDB_BIN}" -no-init -csv -noheader "${RESULTS}/quackapi_crash.duckdb" -c "SELECT count(*) FROM quackapi_jobs WHERE queue = 'bench_jobs';")"
  else
    sleep "$(python3 -c "print(${CRASH_DELAY_MS}/1000 + 0.25)")"
    survived=0
    if [[ -f "${RESULTS}/fastapi_survivors.txt" ]]; then
      survived="$(wc -l <"${RESULTS}/fastapi_survivors.txt" | tr -d ' ')"
    fi
  fi
  python3 - "${stack}" "${CRASH_JOBS}" "${acknowledged}" "${survived}" "${client_json}" <<'PY' >>"${RESULTS}/crash.jsonl"
import json, sys
stack, attempted, acknowledged, survived, raw = sys.argv[1:]
print(json.dumps({"stack": stack, "attempted": int(attempted), "acknowledged": int(acknowledged), "survived": int(survived), "client": json.loads(raw)}, separators=(",", ":")))
PY
}

prepare
record_environment
printf '' >"${RESULTS}/measurements.jsonl"
printf '' >"${RESULTS}/conformance.jsonl"
printf '' >"${RESULTS}/crash.jsonl"

trial=1
while [[ "${trial}" -le "${TRIALS}" ]]; do
  if (( trial % 2 == 1 )); then
    run_stack_trial quackapi "${trial}"
    run_stack_trial fastapi "${trial}"
  else
    run_stack_trial fastapi "${trial}"
    run_stack_trial quackapi "${trial}"
  fi
  trial=$((trial + 1))
done

run_crash_case quackapi
run_crash_case fastapi
(
  cd "${RESULTS}"
  "${DUCKDB_BIN}" -no-init -box <"${BENCH_DIR}/report.sql" | tee report.txt
)
echo "results=${RESULTS}"
