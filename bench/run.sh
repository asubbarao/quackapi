#!/usr/bin/env bash
# Orchestrate side-by-side k6 benchmarks for quackapi (:8000) and fastapi (:8001).
# Both stacks hit the same pgEdge Postgres (no local materialization of bench_rows).
# NEVER runs both stacks at once — serial by design so core contention does not poison numbers.
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS="${BENCH_DIR}/results"
SCENARIOS_DIR="${BENCH_DIR}/scenarios"
K6="${K6:-/opt/homebrew/bin/k6}"
DUCKDB_BIN="${DUCKDB_BIN:-/Users/aloksubbarao/personal/quackapi/build/release/duckdb}"
PSQL="${PSQL:-/Applications/Postgres.app/Contents/Versions/latest/bin/psql}"
export PGPASSWORD="${PGPASSWORD:-password}"
PG_HOST="${PG_HOST:-127.0.0.1}"
PG_PORT="${PG_PORT:-6432}"
PG_USER="${PG_USER:-admin}"
PG_DB="${PG_DB:-quackbench}"

# Stage durations (also documented in README.md). Overridable for smoke tests.
export WARMUP_DURATION="${WARMUP_DURATION:-5s}"
export MEASURE_DURATION="${MEASURE_DURATION:-20s}"
export ROWS_N="${ROWS_N:-1000}"
DEFAULT_VUS="${DEFAULT_VUS:-32}"
# item scenario concurrency sweep (throughput collapse is the finding)
ITEM_VUS_LIST="${ITEM_VUS_LIST:-1 8 16 32}"
# write scenario concurrency sweep
WRITE_VUS_LIST="${WRITE_VUS_LIST:-1 8 16 32 64}"
READY_TIMEOUT_SEC="${READY_TIMEOUT_SEC:-90}"
PORT_FREE_TIMEOUT_SEC="${PORT_FREE_TIMEOUT_SEC:-30}"
BENCH_ROWS_EXPECTED=100000

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

  quackapi-w8 = 8 full DuckDB processes (each LOAD + ATTACH + routes),
  thin RR proxy on the stack port — same multi-process idea as uvicorn -w 8.

Env:
  WARMUP_DURATION   default 5s
  MEASURE_DURATION  default 20s
  ROWS_N            default 1000
  DEFAULT_VUS       VUs for hello/rows (default 32)
  ITEM_VUS_LIST     space-separated VUs for item (default "1 8 16 32")
  WRITE_VUS_LIST    space-separated VUs for write (default "1 8 16 32 64")
  READY_TIMEOUT_SEC server ready poll timeout (default 90)
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
  "$PSQL" -h "$PG_HOST" -p "$PG_PORT" -U "$PG_USER" -d "$PG_DB" -v ON_ERROR_STOP=1 -tAc "$1"
}

precondition_pgedge() {
  local count
  echo "==> precondition: pgEdge reachable and bench_rows == ${BENCH_ROWS_EXPECTED}"
  if ! count="$(psql_q "SELECT count(*)::bigint FROM bench_rows" 2>/dev/null)"; then
    echo "error: pgEdge not answering at ${PG_HOST}:${PG_PORT}/${PG_DB}." >&2
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
  "$PSQL" -h "$PG_HOST" -p "$PG_PORT" -U "$PG_USER" -d "$PG_DB" \
    -v ON_ERROR_STOP=1 -q -c "TRUNCATE bench_writes" >/dev/null
}

count_bench_writes() {
  local n
  n="$(psql_q "SELECT count(*)::bigint FROM bench_writes")"
  echo "$n" | tr -d '[:space:]'
}

# k6 successful-request count from --summary-export JSON.
# Prefer root_group.checks["status 2xx"].passes; fall back to non-failed http_reqs.
k6_successful_reqs() {
  local summary_json="$1"
  python3 - "$summary_json" <<'PY'
import json, sys
path = sys.argv[1]
with open(path) as f:
    d = json.load(f)
checks = (d.get("root_group") or {}).get("checks") or {}
c2 = checks.get("status 2xx")
if isinstance(c2, dict) and "passes" in c2:
    print(int(c2["passes"]))
    raise SystemExit(0)
m = d.get("metrics") or {}
# Rate metric: passes = times failed==true, fails = times failed==false
hrf = m.get("http_req_failed") or {}
if "fails" in hrf:
    print(int(hrf["fails"]))
    raise SystemExit(0)
total = int((m.get("http_reqs") or {}).get("count") or 0)
failed = int(hrf.get("passes") or 0)
print(total - failed)
PY
}

write_rowcheck() {
  local stack="$1" vus="$2" summary_json="$3"
  local pg_rows k6_ok out
  pg_rows="$(count_bench_writes)"
  k6_ok="$(k6_successful_reqs "$summary_json")"
  out="${RESULTS}/${stack}__write__vus${vus}__rowcheck.txt"
  printf '%s %s\n' "$pg_rows" "$k6_ok" >"$out"
  echo "==> rowcheck ${stack} write VUS=${vus}: pg_rows=${pg_rows} k6_ok=${k6_ok} -> ${out}"
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

kill_listeners_on_port() {
  local port="$1"
  local pids
  pids="$(lsof -nP -iTCP:"${port}" -sTCP:LISTEN -t 2>/dev/null || true)"
  if [[ -n "$pids" ]]; then
    # shellcheck disable=SC2086
    kill $pids 2>/dev/null || true
    sleep 0.5
    pids="$(lsof -nP -iTCP:"${port}" -sTCP:LISTEN -t 2>/dev/null || true)"
    if [[ -n "$pids" ]]; then
      # shellcheck disable=SC2086
      kill -9 $pids 2>/dev/null || true
    fi
  fi
}

stop_server() {
  local port="$1"
  if [[ -n "${SERVER_PID:-}" ]]; then
    # Kill the whole process group started with setsid/set -m when possible.
    kill "$SERVER_PID" 2>/dev/null || true
    # Children of the shell that launched the serve script (proxy + duckdb workers).
    pkill -P "$SERVER_PID" 2>/dev/null || true
    # Grandchildren (duckdb under the serve shell).
    local c
    for c in $(pgrep -P "$SERVER_PID" 2>/dev/null || true); do
      pkill -P "$c" 2>/dev/null || true
    done
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
  fi
  kill_listeners_on_port "$port"
  # Multi-process quackapi workers listen on port+1..port+8.
  local wp
  for wp in $(seq $((port + 1)) $((port + 8))); do
    kill_listeners_on_port "$wp"
  done
  wait_until_port_free "$port" "$PORT_FREE_TIMEOUT_SEC" || true
  # Hard re-check: if still up, force and wait again.
  if port_listening "$port"; then
    kill_listeners_on_port "$port"
    wait_until_port_free "$port" "$PORT_FREE_TIMEOUT_SEC"
  fi
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

  # Ensure port is free before boot.
  kill_listeners_on_port "$port"
  wait_until_port_free "$port" 10 || true

  : >"$log"
  # New process group so stop_server can tear down duckdb/uvicorn children.
  set -m
  if [[ -n "$workers" ]]; then
    WORKERS="$workers" bash "$script" >>"$log" 2>&1 &
  else
    bash "$script" >>"$log" 2>&1 &
  fi
  SERVER_PID=$!
  set +m

  echo "==> started ${stack} (pid ${SERVER_PID}) on :${port}${workers:+ workers=${workers}}; waiting up to ${READY_TIMEOUT_SEC}s"
  if ! wait_until_ready "$stack" "$port" "$READY_TIMEOUT_SEC"; then
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
    echo "uname: $(uname -a)"
    echo -n "hw.model: "; sysctl -n hw.model 2>/dev/null || echo "unknown"
    echo -n "hw.ncpu: "; sysctl -n hw.ncpu 2>/dev/null || echo "unknown"
    echo "k6: $($K6 version 2>&1 | head -n 1)"
    # .duckdbrc may print "Loading resources..." on stderr/stdout — keep only the version line.
    echo "duckdb_cli: $($DUCKDB_BIN -init /dev/null --version 2>/dev/null | grep -E '^v?[0-9]' | head -n 1)"
    echo "warmup_duration: ${WARMUP_DURATION}"
    echo "measure_duration: ${MEASURE_DURATION}"
    echo "default_vus: ${DEFAULT_VUS}"
    echo "item_vus_list: ${ITEM_VUS_LIST}"
    echo "write_vus_list: ${WRITE_VUS_LIST}"
    echo "rows_n: ${ROWS_N}"
    echo "pg_dsn: postgresql://${PG_USER}@${PG_HOST}:${PG_PORT}/${PG_DB}"
    # pgEdge surface (required for the Postgres-backed comparison).
    pg_server_version="$(psql_q "SHOW server_version" 2>/dev/null || echo "unavailable")"
    pg_max_conn="$(psql_q "SHOW max_connections" 2>/dev/null || echo "unavailable")"
    pg_spock="$(psql_q "SELECT extversion FROM pg_extension WHERE extname='spock'" 2>/dev/null || true)"
    pg_spock="$(echo "${pg_spock:-}" | tr -d '[:space:]')"
    echo "pgedge_server_version: ${pg_server_version}"
    echo "pgedge_max_connections: ${pg_max_conn}"
    echo "pgedge_spock_extversion: ${pg_spock:-<not installed>}"
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
    "$K6" run --summary-export "$out" --out "csv=${raw}" "$script"
  local rc=$?
  set -e
  if [[ ! -f "$out" ]]; then
    echo "error: k6 did not write summary export ${out} (exit ${rc})" >&2
    exit 1
  fi
  if [[ $rc -ne 0 ]]; then
    echo "warning: k6 exited ${rc} for ${stack}/${scenario} VUS=${vus} — summary kept; check fail rate in report" >&2
  fi
}

# ---- main ----
precondition_pgedge

echo "==> clearing stale results in ${RESULTS}"
rm -rf "$RESULTS"
mkdir -p "$RESULTS"

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
trap 'if [[ -n "${SERVER_PID:-}" ]]; then stop_server 8000; stop_server 8001; for p in $(seq 8001 8016); do kill_listeners_on_port "$p" 2>/dev/null || true; done; fi' EXIT

for stack in "${STACKS[@]}"; do
  port="$(port_for_stack "$stack")"
  start_server "$stack"

  for scenario in "${SCENARIOS[@]}"; do
    case "$scenario" in
      write)
        # shellcheck disable=SC2206
        write_vus_arr=($WRITE_VUS_LIST)
        for vus in "${write_vus_arr[@]}"; do
          echo "==> TRUNCATE bench_writes before ${stack} write VUS=${vus}"
          truncate_bench_writes
          run_k6 "$stack" write "$vus" "${stack}__write__vus${vus}"
          write_rowcheck "$stack" "$vus" "${RESULTS}/${stack}__write__vus${vus}.json"
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
  cd "$BENCH_DIR"
  "$DUCKDB_BIN" -init /dev/null -c ".read report.sql" 2>/dev/null || {
    echo "warning: report.sql failed (naming may have changed); raw k6 JSON still in ${RESULTS}/" >&2
  }
)

echo "==> done. Results in ${RESULTS}/"
echo "    env:     ${RESULTS}/env.txt"
echo "    naming:  <stack>__{hello,rows}.json"
echo "             <stack>__item__vus{N}.json"
echo "             <stack>__write__vus{N}.json"
echo "             <stack>__write__vus{N}__rowcheck.txt  # \"<pg_rows> <k6_ok>\""
echo "    stacks:  quackapi-w1 | quackapi-w8 | fastapi-w1 | fastapi-w8"
echo "    report:  re-run with: ${DUCKDB_BIN} -init /dev/null -c '.read ${BENCH_DIR}/report.sql'"
