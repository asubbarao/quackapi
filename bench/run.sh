#!/usr/bin/env bash
# Orchestrate side-by-side k6 benchmarks for quackapi (:8000) and fastapi (:8001).
# Both stacks hit the same pgEdge Postgres (no local materialization of bench_rows).
# NEVER runs both stacks at once — serial by design so core contention does not poison numbers.
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_ROOT="${RESULTS_ROOT:-${BENCH_DIR}/results}"
# RESULTS points at one immutable run directory once the executable path is
# entered. Keep it rooted at RESULTS_ROOT while sourced by isolated tests.
RESULTS="${RESULTS_ROOT}"
SCENARIOS_DIR="${BENCH_DIR}/scenarios"
K6="${K6:-/opt/homebrew/bin/k6}"
DUCKDB_BIN="${DUCKDB_BIN:-/Users/aloksubbarao/personal/quackapi/build/release/duckdb}"
PSQL="${PSQL:-/Applications/Postgres.app/Contents/Versions/latest/bin/psql}"
export PGPASSWORD="${PGPASSWORD:-password}"
export PG_HOST="${PG_HOST:-127.0.0.1}"
export PG_PORT="${PG_PORT:-6432}"
export PG_USER="${PG_USER:-admin}"
export PG_DB="${PG_DB:-quackbench}"
PG_DSN="$(python3 "${BENCH_DIR}/bench_config.py")"
export PG_DSN

# Stage durations (also documented in README.md). Overridable for smoke tests.
export WARMUP_DURATION="${WARMUP_DURATION:-5s}"
export MEASURE_DURATION="${MEASURE_DURATION:-20s}"
# Time for write warmup VUs to finish their final request before the separately
# scheduled measurement phase begins. This avoids a shared-VU starvation race.
export WRITE_WARMUP_DRAIN_DURATION="${WRITE_WARMUP_DRAIN_DURATION:-2s}"
# POSTs must finish before k6 exits or an acknowledged client request can lag
# a committed server transaction. A long grace only extends an overloaded run.
export WRITE_MEASURE_GRACEFUL_STOP_DURATION="${WRITE_MEASURE_GRACEFUL_STOP_DURATION:-30s}"
export ROWS_N="${ROWS_N:-1000}"
DEFAULT_VUS="${DEFAULT_VUS:-32}"
# item scenario concurrency sweep (throughput collapse is the finding)
ITEM_VUS_LIST="${ITEM_VUS_LIST:-1 8 16 32}"
# write scenario concurrency sweep
WRITE_VUS_LIST="${WRITE_VUS_LIST:-1 8 16 32 64}"
READY_TIMEOUT_SEC="${READY_TIMEOUT_SEC:-90}"
PORT_FREE_TIMEOUT_SEC="${PORT_FREE_TIMEOUT_SEC:-30}"
BENCH_ROWS_EXPECTED=100000
RUN_INVALID=0

# Logical stacks: quackapi-w1/w8 (process count like uvicorn); fastapi-w1/w8.
ALL_STACKS=(quackapi-w1 quackapi-w8 fastapi-w1 fastapi-w8)
ALL_SCENARIOS=(hello item rows write)

STACKS=()
SCENARIOS=()

usage() {
  cat <<'EOF'
Usage: bench/run.sh [stack|scenario]...

  stacks:     quackapi  quackapi-w1  quackapi-w8
              fastapi   fastapi-w1   fastapi-w8
              (quackapi / fastapi expand to both -w1 and -w8)
  scenarios:  hello  item  rows  write

  Defaults to all stacks and all scenarios.
  Stacks always run serially (never concurrent).
  item is swept across VUS levels (default: 1 8 16 32).
  write is swept across VUS levels (default: 1 8 16 32 64).
  hello and rows use DEFAULT_VUS (default 32).

  quackapi-w8 = 8 full DuckDB processes sharing the stack port via SO_REUSEPORT.

Env:
  WARMUP_DURATION   default 5s
  MEASURE_DURATION  default 20s
  WRITE_WARMUP_DRAIN_DURATION  default 2s; wait for write warmup requests
                               before measurement VUs start
  WRITE_MEASURE_GRACEFUL_STOP_DURATION  default 30s; wait for final write
                               requests before k6 exits
  ROWS_N            default 1000
  DEFAULT_VUS       VUs for hello/rows (default 32)
  ITEM_VUS_LIST     space-separated VUs for item (default "1 8 16 32")
  WRITE_VUS_LIST    space-separated VUs for write (default "1 8 16 32 64")
  READY_TIMEOUT_SEC server ready poll timeout (default 90)
  PG_DSN            shared by preflight, both servers, and write verification;
                    otherwise built from PG_HOST/PORT/USER/DB and PGPASSWORD
EOF
}

for arg in "$@"; do
  case "$arg" in
    -h|--help) usage; exit 0 ;;
    quackapi) STACKS+=("quackapi-w1" "quackapi-w8") ;;
    quackapi-w1|quackapi-w8) STACKS+=("$arg") ;;
    fastapi) STACKS+=("fastapi-w1" "fastapi-w8") ;;
    fastapi-w1|fastapi-w8) STACKS+=("$arg") ;;
    hello|item|rows|write) SCENARIOS+=("$arg") ;;
    *)
      echo "error: unknown argument '$arg' (expected stack or scenario name)" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ ${#STACKS[@]} -eq 0 ]]; then
  STACKS=("${ALL_STACKS[@]}")
fi
if [[ ${#SCENARIOS[@]} -eq 0 ]]; then
  SCENARIOS=("${ALL_SCENARIOS[@]}")
fi

# Deduplicate stacks while preserving order (fastapi expands to two tokens).
if [[ ${#STACKS[@]} -gt 0 ]]; then
  _seen=""
  _deduped=()
  for s in "${STACKS[@]}"; do
    case " ${_seen} " in
      *" ${s} "*) ;;
      *)
        _deduped+=("$s")
        _seen="${_seen} ${s}"
        ;;
    esac
  done
  STACKS=("${_deduped[@]}")
  unset _seen _deduped s
fi

if [[ ! -x "$K6" ]]; then
  echo "error: k6 not found or not executable: $K6" >&2
  exit 1
fi
if [[ ! -x "$DUCKDB_BIN" ]]; then
  echo "error: duckdb CLI not found or not executable: $DUCKDB_BIN" >&2
  exit 1
fi
if [[ ! -x "$PSQL" ]]; then
  echo "error: psql not found or not executable: $PSQL" >&2
  exit 1
fi

# ---- pgEdge helpers ----
psql_q() {
  # One SQL statement; -tAc → bare cell(s). Connection failure / SQL error → non-zero.
  "$PSQL" "$PG_DSN" -v ON_ERROR_STOP=1 -tAc "$1"
}

precondition_pgedge() {
  local count
  echo "==> precondition: pgEdge reachable and bench_rows == ${BENCH_ROWS_EXPECTED}"
  if ! count="$(psql_q "SELECT count(*)::bigint FROM bench_rows" 2>/dev/null)"; then
    echo "error: pgEdge not answering at the configured PG_DSN." >&2
    echo "error: bring it up with: podman start pgedge-n1" >&2
    exit 1
  fi
  count="$(echo "$count" | tr -d '[:space:]')"
  if [[ "$count" != "$BENCH_ROWS_EXPECTED" ]]; then
    echo "error: bench_rows has ${count:-<empty>} rows; expected ${BENCH_ROWS_EXPECTED}." >&2
    echo "error: refusing to produce results against a dead or empty database." >&2
    echo "error: if pgEdge is down: podman start pgedge-n1" >&2
    exit 1
  fi
  echo "==> precondition ok: bench_rows=${count}"
}

truncate_bench_writes() {
  # -q + discard stdout: TRUNCATE still emits a command tag under -tAc.
  "$PSQL" "$PG_DSN" \
    -v ON_ERROR_STOP=1 -q -c "TRUNCATE bench_writes" >/dev/null
}

count_bench_writes() {
  local n
  n="$(psql_q "SELECT count(*)::bigint FROM bench_writes")"
  echo "$n" | tr -d '[:space:]'
}

# Count only successful measurement writes. Warmup posts may still be draining
# when their own scenario ends, so the row-level commit/ack invariant must use
# the explicit measurement counter, not a whole-run request total.
k6_successful_reqs() {
  local summary_json="$1"
  python3 - "$summary_json" <<'PY'
import json, sys
path = sys.argv[1]
with open(path) as f:
    d = json.load(f)
m = d.get("metrics") or {}
counter = m.get("write_successful_measure") or {}
print(int(counter.get("count") or 0))
PY
}

k6_measure_summary() {
  # Emit one stable tab-separated row consumed by report.sql. Failed HTTP
  # responses and check failures remain visible even when k6 thresholds are
  # deliberately non-blocking so a bad cell can be retained and labelled.
  local summary_json="$1" rc="$2" stack="$3" scenario="$4" vus="$5" export_name="$6"
  python3 - "$summary_json" "$rc" "$stack" "$scenario" "$vus" "$export_name" <<'PY'
import json, os, sys

path, rc, stack, scenario, vus, export_name = sys.argv[1:]
with open(path) as f:
    d = json.load(f)
metrics = d.get("metrics") or {}

def metric(name, stage=True):
    if stage:
        staged = metrics.get(name + "{stage:measure}")
        if isinstance(staged, dict):
            return staged
    value = metrics.get(name)
    return value if isinstance(value, dict) else {}

req = metric("http_reqs")
failed = metric("http_req_failed")
checks = metric("checks")
attempted = int(req.get("count") or 0)
http_failed = int(failed.get("passes") or 0)
successful = max(0, attempted - http_failed)
check_failures = int(checks.get("fails") or 0)
all_req = metrics.get("http_reqs") or {}
all_failed = metrics.get("http_req_failed") or {}
all_attempted = int(all_req.get("count") or 0)
all_http_failed = int(all_failed.get("passes") or 0)
all_successful = max(0, all_attempted - all_http_failed)
reasons = []
if int(rc) != 0:
    reasons.append("k6_exit=%s" % rc)
if attempted == 0:
    reasons.append("no_measure_requests")
if http_failed:
    reasons.append("measure_http_failures=%d" % http_failed)
if check_failures:
    reasons.append("measure_check_failures=%d" % check_failures)
valid = int(not reasons)
duration_text = os.environ.get("MEASURE_DURATION", "20s")
try:
    if duration_text.endswith("ms"):
        measure_seconds = float(duration_text[:-2]) / 1000.0
    elif duration_text.endswith("m"):
        measure_seconds = float(duration_text[:-1]) * 60.0
    else:
        measure_seconds = float(duration_text.rstrip("s"))
except ValueError:
    measure_seconds = 0.0
fields = [
    export_name, stack, scenario, vus, rc, attempted, successful,
    http_failed, check_failures, valid, ";".join(reasons) or "",
    all_attempted, all_successful, all_http_failed, measure_seconds,
]
print("\t".join(str(x) for x in fields))
PY
}

count_bench_writes_for_prefix() {
  local prefix="$1" sql_prefix
  sql_prefix="${prefix//\'/\'\'}"
  # write.js reserves phase 1 IDs for measurement. Excluding warmup lets this
  # compare committed measurement rows exactly to the measurement-only k6
  # counter even when warmup had legitimately committed rows.
  psql_q "SELECT count(*)::bigint FROM bench_writes WHERE note LIKE '${sql_prefix}-%' AND id >= 1000000000000"
}

write_rowcheck() {
  local stack="$1" vus="$2" summary_json="$3" prefix="$4"
  local pg_rows k6_ok out row_status
  pg_rows="$(count_bench_writes_for_prefix "$prefix")"
  k6_ok="$(k6_successful_reqs "$summary_json")"
  out="${RESULTS}/${stack}__write__vus${vus}__rowcheck.txt"
  row_status="valid"
  if [[ "$pg_rows" != "$k6_ok" ]]; then
    row_status="invalid"
    RUN_INVALID=1
  fi
  printf 'prefix=%s\npg_rows=%s\nk6_ok=%s\nstatus=%s\n' \
    "$prefix" "$pg_rows" "$k6_ok" "$row_status" >"$out"
  printf '%s\t%s\t%s\t%s\t%s\n' "$stack" write "$vus" "$pg_rows" "$k6_ok" >>"${RESULTS}/rowchecks.tsv"
  echo "==> rowcheck ${stack} write VUS=${vus}: pg_rows=${pg_rows} k6_ok=${k6_ok} status=${row_status} -> ${out}"
}

port_for_stack() {
  case "$1" in
    quackapi|quackapi-w1|quackapi-w8) echo 8000 ;;
    fastapi|fastapi-w1|fastapi-w8) echo 8001 ;;
    *) echo "error: unknown stack $1" >&2; return 1 ;;
  esac
}

serve_script_for_stack() {
  case "$1" in
    quackapi|quackapi-w1|quackapi-w8) echo "${BENCH_DIR}/serve_quackapi.sh" ;;
    fastapi|fastapi-w1|fastapi-w8) echo "${BENCH_DIR}/serve_fastapi.sh" ;;
  esac
}

workers_for_stack() {
  case "$1" in
    quackapi-w8|fastapi-w8) echo 8 ;;
    quackapi-w1|quackapi|fastapi-w1|fastapi) echo 1 ;;
    *) echo "" ;;
  esac
}

record_effective_settings() {
  local stack="$1" workers="$2" http_threads pg_pool duck_threads
  if [[ "$workers" -gt 1 ]]; then
    http_threads=4
    pg_pool=4
    duck_threads=1
  else
    http_threads=32
    pg_pool=32
    duck_threads=4
  fi
  if [[ "$stack" == fastapi-* ]]; then
    duck_threads="n/a"
  fi
  printf '%s\t%s\t%s\t%s\t%s\n' "$stack" "$workers" "$http_threads" "$pg_pool" "$duck_threads" >>"${RESULTS}/effective_settings.tsv"
}

# Return 0 if /hello answers on the port.
port_ready() {
  local port="$1"
  curl -sf --max-time 1 "http://127.0.0.1:${port}/hello" >/dev/null 2>&1
}

port_listening() {
  local port="$1"
  # macOS: lsof is reliable; avoid racing curl against a half-dead process.
  lsof -nP -iTCP:"${port}" -sTCP:LISTEN >/dev/null 2>&1
}

wait_until_ready() {
  local stack="$1" port="$2" timeout="$3"
  local start now
  start="$(date +%s)"
  while true; do
    if port_ready "$port"; then
      return 0
    fi
    now="$(date +%s)"
    if (( now - start >= timeout )); then
      echo "error: stack '${stack}' did not become ready on 127.0.0.1:${port} within ${timeout}s" >&2
      echo "error: last server log (${RESULTS}/${stack}__server.log):" >&2
      if [[ -f "${RESULTS}/${stack}__server.log" ]]; then
        tail -n 80 "${RESULTS}/${stack}__server.log" >&2 || true
      else
        echo "  (log file missing)" >&2
      fi
      return 1
    fi
    # If the process already died, fail fast with the log.
    if [[ -n "${SERVER_PID:-}" ]] && ! kill -0 "$SERVER_PID" 2>/dev/null; then
      echo "error: stack '${stack}' process (pid ${SERVER_PID}) exited before becoming ready" >&2
      echo "error: server log:" >&2
      tail -n 80 "${RESULTS}/${stack}__server.log" >&2 || true
      return 1
    fi
    sleep 0.25
  done
}

wait_until_port_free() {
  local port="$1" timeout="$2"
  local start now
  start="$(date +%s)"
  while port_listening "$port"; do
    now="$(date +%s)"
    if (( now - start >= timeout )); then
      echo "error: port ${port} still listening after ${timeout}s — cannot start next stack cleanly" >&2
      return 1
    fi
    sleep 0.25
  done
}

verify_worker_pids() {
  local stack="$1" expected="$2" log="$3" line pids pid count start
  if [[ "$expected" -le 1 ]]; then
    kill -0 "$SERVER_PID" 2>/dev/null
    return
  fi
  # QuackAPI's first worker can answer /hello before the wrapper has launched
  # and logged every sibling. Wait for the authoritative PID set instead of
  # turning that normal startup race into a failed benchmark cell.
  start="$(date +%s)"
  while true; do
    line="$(grep 'worker_pids=' "$log" | tail -n 1 || true)"
    if [[ -n "$line" ]]; then
      pids="${line#*worker_pids=}"
      count=0
      for pid in $pids; do
        if ! kill -0 "$pid" 2>/dev/null; then
          echo "error: ${stack} worker pid ${pid} is not alive" >&2
          return 1
        fi
        count=$((count + 1))
      done
      if [[ "$count" -eq "$expected" ]]; then
        echo "==> verified ${stack}: ${count} worker processes alive"
        return 0
      fi
    fi
    if [[ -n "${SERVER_PID:-}" ]] && ! kill -0 "$SERVER_PID" 2>/dev/null; then
      echo "error: ${stack} process exited before publishing ${expected} workers" >&2
      return 1
    fi
    if (( $(date +%s) - start >= READY_TIMEOUT_SEC )); then
      echo "error: ${stack} requested ${expected} workers but did not publish a complete live PID set" >&2
      return 1
    fi
    sleep 0.1
  done
}

stop_server() {
  local port="$1"
  if [[ -n "${SERVER_PID:-}" ]]; then
    # start_server uses job control to give this launch its own process group.
    # Only that group belongs to us; a listening port is never proof of ownership.
    local group="$SERVER_PID" start
    kill -TERM -- "-${group}" 2>/dev/null || true
    start="$(date +%s)"
    while kill -0 -- "-${group}" 2>/dev/null; do
      if (( $(date +%s) - start >= PORT_FREE_TIMEOUT_SEC )); then
        kill -KILL -- "-${group}" 2>/dev/null || true
        break
      fi
      sleep 0.1
    done
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
    SERVER_PORT=""
  fi
  wait_until_port_free "$port" "$PORT_FREE_TIMEOUT_SEC"
}

start_server() {
  local stack="$1"
  local port script log workers
  port="$(port_for_stack "$stack")"
  script="$(serve_script_for_stack "$stack")"
  log="${RESULTS}/${stack}__server.log"
  workers="$(workers_for_stack "$stack")"

  if [[ ! -x "$script" && ! -f "$script" ]]; then
    echo "error: serve script missing for ${stack}: ${script}" >&2
    echo "error: another agent owns that file — it must exist before a full run." >&2
    exit 1
  fi
  if [[ ! -x "$script" ]]; then
    chmod +x "$script" || true
  fi

  if port_listening "$port"; then
    echo "error: port ${port} is already occupied; refusing to stop an unrelated listener" >&2
    return 1
  fi

  record_effective_settings "$stack" "$workers"
  : >"$log"
  # New process group so stop_server can tear down duckdb/uvicorn children.
  set -m
  if [[ -n "$workers" ]]; then
    WORKERS="$workers" bash "$script" >>"$log" 2>&1 &
  else
    bash "$script" >>"$log" 2>&1 &
  fi
  SERVER_PID=$!
  SERVER_PORT="$port"
  set +m

  echo "==> started ${stack} (pid ${SERVER_PID}) on :${port}${workers:+ workers=${workers}}; waiting up to ${READY_TIMEOUT_SEC}s"
  if ! wait_until_ready "$stack" "$port" "$READY_TIMEOUT_SEC"; then
    stop_server "$port"
    exit 1
  fi
  if ! verify_worker_pids "$stack" "$workers" "$log"; then
    stop_server "$port"
    exit 1
  fi
  echo "==> ${stack} ready on :${port}"
}

record_env() {
  local env_file="${RESULTS}/env.txt"
  local pg_server_version pg_max_conn pg_spock
  {
    echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "run_id: ${RUN_ID}"
    echo "run_dir: ${RESULTS}"
    echo "source_git_sha: $(git -C "${BENCH_DIR}/.." rev-parse HEAD 2>/dev/null || echo unknown)"
    if git -C "${BENCH_DIR}/.." diff --quiet -- bench 2>/dev/null; then
      echo "source_git_dirty: false"
    else
      echo "source_git_dirty: true"
    fi
    echo "uname: $(uname -a)"
    echo -n "hw.model: "; sysctl -n hw.model 2>/dev/null || echo "unknown"
    echo -n "hw.ncpu: "; sysctl -n hw.ncpu 2>/dev/null || echo "unknown"
    echo "k6: $($K6 version 2>&1 | head -n 1)"
    # .duckdbrc may print "Loading resources..." on stderr/stdout — keep only the version line.
    echo "duckdb_cli: $($DUCKDB_BIN -init /dev/null --version 2>/dev/null | grep -E '^v?[0-9]' | head -n 1)"
    echo "warmup_duration: ${WARMUP_DURATION}"
    echo "measure_duration: ${MEASURE_DURATION}"
    echo "write_warmup_drain_duration: ${WRITE_WARMUP_DRAIN_DURATION}"
    echo "write_measure_graceful_stop_duration: ${WRITE_MEASURE_GRACEFUL_STOP_DURATION}"
    echo "default_vus: ${DEFAULT_VUS}"
    echo "item_vus_list: ${ITEM_VUS_LIST}"
    echo "write_vus_list: ${WRITE_VUS_LIST}"
    echo "rows_n: ${ROWS_N}"
    echo "stack_order: ${STACKS[*]}"
    echo "scenario_order: ${SCENARIOS[*]}"
    echo "resource_budget: w1=32_http_workers/32_pg_connections; w8=32_http_workers/32_pg_connections"
    echo "pg_dsn: shared configured connection (credentials omitted)"
    # pgEdge surface (required for the Postgres-backed comparison).
    pg_server_version="$(psql_q "SHOW server_version" 2>/dev/null || echo "unavailable")"
    pg_max_conn="$(psql_q "SHOW max_connections" 2>/dev/null || echo "unavailable")"
    pg_spock="$(psql_q "SELECT extversion FROM pg_extension WHERE extname='spock'" 2>/dev/null || true)"
    pg_spock="$(echo "${pg_spock:-}" | tr -d '[:space:]')"
    echo "pgedge_server_version: ${pg_server_version}"
    echo "pgedge_max_connections: ${pg_max_conn}"
    echo "pgedge_spock_extversion: ${pg_spock:-<not installed>}"
    echo "duckdb_binary: ${DUCKDB_BIN}"
    echo "duckdb_sha256: $(shasum -a 256 "${DUCKDB_BIN}" 2>/dev/null | awk '{print $1}' || echo unavailable)"
    local_extension="${QUACKAPI_EXT:-${BENCH_DIR}/../build/release/extension/quackapi/quackapi.duckdb_extension}"
    echo "quackapi_extension: ${local_extension}"
    echo "quackapi_extension_sha256: $(shasum -a 256 "${local_extension}" 2>/dev/null | awk '{print $1}' || echo unavailable)"
    if [[ -f "${RESULTS}/fastapi_versions.txt" ]]; then
      echo "--- fastapi_versions.txt ---"
      cat "${RESULTS}/fastapi_versions.txt"
    fi
    # Capture Python stack versions from the bench venv when present.
    if [[ -x "${BENCH_DIR}/.venv/bin/python" ]]; then
      echo "--- python stack (bench/.venv) ---"
      "${BENCH_DIR}/.venv/bin/python" - <<'PY' 2>/dev/null || true
import sys
try:
    import fastapi, uvicorn
    print(f"python: {sys.version.split()[0]}")
    print(f"fastapi: {fastapi.__version__}")
    print(f"uvicorn: {uvicorn.__version__}")
    try:
        import psycopg
        print(f"psycopg: {psycopg.__version__}")
    except Exception:
        pass
    try:
        import psycopg_pool
        print(f"psycopg_pool: {psycopg_pool.__version__}")
    except Exception:
        pass
except Exception as e:
    print(f"python_stack_error: {e}")
PY
    fi
  } >"$env_file"
  echo "==> wrote ${env_file}"
}

run_k6() {
  local stack="$1" scenario="$2" vus="$3" export_name="$4"
  local port base_url script out
  port="$(port_for_stack "$stack")"
  base_url="http://127.0.0.1:${port}"
  script="${SCENARIOS_DIR}/${scenario}.js"
  out="${RESULTS}/${export_name}.json"

  if [[ ! -f "$script" ]]; then
    echo "error: missing k6 scenario script: ${script}" >&2
    exit 1
  fi

  # Raw per-request samples (one row per metric point). This is the source of
  # truth — the full latency distribution is computed at read time in DuckDB.
  # The --summary-export JSON is kept only for the cheap check-fail rate.
  local raw="${RESULTS}/raw/${export_name}.csv.gz"
  mkdir -p "${RESULTS}/raw"

  echo "==> k6 ${stack} ${scenario} VUS=${vus} -> raw ${raw}"
  set +e
  BASE_URL="$base_url" \
  VUS="$vus" \
  ROWS_N="$ROWS_N" \
  WARMUP_DURATION="$WARMUP_DURATION" \
  MEASURE_DURATION="$MEASURE_DURATION" \
  WRITE_WARMUP_DRAIN_DURATION="$WRITE_WARMUP_DRAIN_DURATION" \
  WRITE_MEASURE_GRACEFUL_STOP_DURATION="$WRITE_MEASURE_GRACEFUL_STOP_DURATION" \
  WRITE_PREFIX="${WRITE_PREFIX:-}" \
    "$K6" run --summary-export "$out" --out "csv=${raw}" "$script"
  local rc=$?
  set -e
  if [[ ! -f "$out" ]]; then
    echo "error: k6 did not write summary export ${out} (exit ${rc})" >&2
    exit 1
  fi
  if [[ $rc -ne 0 ]]; then
    echo "warning: k6 exited ${rc} for ${stack}/${scenario} VUS=${vus} — cell is invalid; summary kept" >&2
    RUN_INVALID=1
  fi
  if [[ ! -f "${RESULTS}/cells.tsv" ]]; then
    printf 'export_name\tstack\tscenario\tvus\tk6_exit\tmeasure_requests\tmeasure_successful\tmeasure_http_failures\tmeasure_check_failures\tvalid\tinvalid_reason\tall_requests\tall_successful\tall_http_failures\tmeasure_seconds\n' >"${RESULTS}/cells.tsv"
  fi
  k6_measure_summary "$out" "$rc" "$stack" "$scenario" "$vus" "$export_name" >>"${RESULTS}/cells.tsv"
}

init_run_artifacts() {
  mkdir -p "$RESULTS_ROOT"
  if [[ -z "${RUN_ID:-}" ]]; then
    RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
  fi
  RESULTS="${RESULTS_ROOT}/${RUN_ID}"
  if [[ -e "$RESULTS" ]]; then
    echo "error: run directory already exists: ${RESULTS}" >&2
    exit 1
  fi
  mkdir -p "${RESULTS}/raw"
  printf 'run_id=%s\nrun_dir=%s\n' "$RUN_ID" "$RESULTS" >"${RESULTS}/manifest.txt"
  printf 'stack\tscenario\tvus\tpg_rows\tk6_ok\n' >"${RESULTS}/rowchecks.tsv"
  printf 'stack\tworkers\thttp_threads_per_process\tpg_pool_per_process\tduckdb_threads\n' >"${RESULTS}/effective_settings.tsv"
}

# ---- main ----
# Permit isolated tests to source the actual helper functions without starting a
# server, checking Postgres, or removing result files.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  return 0
fi
precondition_pgedge
init_run_artifacts
echo "==> preserving prior runs; writing this run to ${RESULTS}"

record_env

# Capture fastapi versions into the contract path if venv exists (used by report env).
if [[ -x "${BENCH_DIR}/.venv/bin/python" ]]; then
  "${BENCH_DIR}/.venv/bin/python" - <<'PY' >"${RESULTS}/fastapi_versions.txt" 2>/dev/null || true
import sys
try:
    import fastapi, uvicorn
    print(f"python={sys.version.split()[0]}")
    print(f"fastapi={fastapi.__version__}")
    print(f"uvicorn={uvicorn.__version__}")
    try:
        import psycopg
        print(f"psycopg={psycopg.__version__}")
    except Exception:
        pass
except Exception as e:
    print(f"error={e}")
PY
  # Refresh env.txt so it includes fastapi_versions.txt
  record_env
fi

SERVER_PID=""
SERVER_PORT=""
trap 'if [[ -n "${SERVER_PID:-}" ]]; then stop_server "$SERVER_PORT"; fi' EXIT

for stack in "${STACKS[@]}"; do
  port="$(port_for_stack "$stack")"
  start_server "$stack"

  for scenario in "${SCENARIOS[@]}"; do
    case "$scenario" in
      write)
        # shellcheck disable=SC2206
        write_vus_arr=($WRITE_VUS_LIST)
        for vus in "${write_vus_arr[@]}"; do
          write_prefix="${RUN_ID}-${stack}-write-vus${vus}"
          echo "==> TRUNCATE bench_writes before ${stack} write VUS=${vus}"
          truncate_bench_writes
          WRITE_PREFIX="$write_prefix" run_k6 "$stack" write "$vus" "${stack}__write__vus${vus}"
          write_rowcheck "$stack" "$vus" "${RESULTS}/${stack}__write__vus${vus}.json" "$write_prefix"
        done
        ;;
      item)
        # shellcheck disable=SC2206
        item_vus_arr=($ITEM_VUS_LIST)
        for vus in "${item_vus_arr[@]}"; do
          run_k6 "$stack" item "$vus" "${stack}__item__vus${vus}"
        done
        ;;
      *)
        run_k6 "$stack" "$scenario" "$DEFAULT_VUS" "${stack}__${scenario}"
        ;;
    esac
  done

  echo "==> stopping ${stack}"
  stop_server "$port"
  echo "==> port ${port} free; next stack may start"
done

echo "==> writing comparison table via report.sql"
(
  cd "$RESULTS"
	"$DUCKDB_BIN" -init /dev/null < "${BENCH_DIR}/report.sql" 2>/dev/null || {
    echo "error: report.sql failed; run artifacts are preserved in ${RESULTS}/" >&2
    exit 1
  }
)

echo "==> done. Results in ${RESULTS}/"
echo "    env:     ${RESULTS}/env.txt"
echo "    naming:  <stack>__{hello,rows}.json"
echo "             <stack>__item__vus{N}.json"
echo "             <stack>__write__vus{N}.json"
echo "             <stack>__write__vus{N}__rowcheck.txt  # scoped commit/ack verification"
echo "    stacks:  quackapi-w1 | quackapi-w8 | fastapi-w1 | fastapi-w8"
echo "    report:  (cd ${RESULTS} && ${DUCKDB_BIN} -init /dev/null < ${BENCH_DIR}/report.sql)"
if [[ "$RUN_INVALID" -ne 0 ]]; then
  echo "error: one or more benchmark cells are invalid; no speed claim is publishable" >&2
  exit 1
fi
