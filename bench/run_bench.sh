#!/bin/bash
# run_bench.sh — honest quackapi vs FastAPI+uvicorn head-to-head
# Re-runnable. Creates/uses /tmp/qbench_venv, /tmp/qbench*.db , ports 18400+
# NEVER touches 9494/9495. Kills ONLY via lsof-exact-PID on OUR ports.
# Replicates B2 methodology exactly: ab -n 8000 -k -c8 and -c64
set -euo pipefail

BENCH_DIR="/Users/aloksubbarao/quackapi/bench"
cd "$BENCH_DIR"

DUCK="/Users/aloksubbarao/.local/bin/duckdb"
EXT="/Users/aloksubbarao/quackapi/ext-cpp/build/release/extension/quackapi/quackapi.duckdb_extension"
FRAMEWORK="/Users/aloksubbarao/quackapi/framework.sql"
APP="/Users/aloksubbarao/quackapi/app.sql"
AB="/usr/sbin/ab"
VENV="/tmp/qbench_venv"
LOGDIR="/tmp"
QDB="/tmp/qbench_quack.db"
FDB="/tmp/qbench_fast.db"

# Ports (18400-18499 only)
P_QUACK=18400
P_MEM1=18410
P_MEMN=18411
P_DUCK1=18420
P_DUCKN=18421

NCPU=$(sysctl -n hw.ncpu)
echo "=== MACHINE CONTEXT ==="
echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "macOS: $(sw_vers -productName) $(sw_vers -productVersion) $(sw_vers -buildVersion)"
echo "hw.ncpu: $NCPU"
echo "hw.model: $(sysctl -n hw.model 2>/dev/null || echo 'n/a')"
echo "ab: $(${AB} -V 2>&1 | head -1)"
echo "duck: $(${DUCK} --version 2>/dev/null | head -1 || echo 'duck')"
echo "======================="

cleanup_port() {
  local port=$1
  local pids
  pids=$(lsof -nP -tiTCP:${port} -sTCP:LISTEN 2>/dev/null || true)
  if [[ -n "$pids" ]]; then
    echo "Killing prior listeners on $port: $pids"
    kill $pids 2>/dev/null || true
    sleep 0.3
  fi
}

get_listen_pid() {
  local port=$1
  lsof -nP -tiTCP:${port} -sTCP:LISTEN 2>/dev/null | head -1
}

wait_for_listen() {
  local port=$1
  local tries=0
  while [[ $tries -lt 120 ]]; do
    if lsof -nP -tiTCP:${port} -sTCP:LISTEN >/dev/null 2>&1; then
      # also verify it actually serves (esp. for multi-worker uvicorn)
      if /usr/bin/curl -s --max-time 2 "http://127.0.0.1:${port}/health" | grep -q 'status'; then
        return 0
      fi
    fi
    sleep 0.1
    tries=$((tries+1))
  done
  echo "FAIL: port $port never listened or served /health" >&2
  # dump tail of its log if exists for debug
  ls -l ${LOGDIR}/qbench_*.log 2>/dev/null || true
  return 1
}

wait_port_free() {
  local port=$1
  local tries=0
  while [[ $tries -lt 30 ]]; do
    if ! lsof -nP -tiTCP:${port} -sTCP:LISTEN >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
    tries=$((tries+1))
  done
  echo "WARN: port $port still busy after kill" >&2
}

curl_body() {
  local port=$1
  local path=$2
  /usr/bin/curl -s --max-time 5 "http://127.0.0.1:${port}${path}" || echo "CURL_FAIL"
}

run_warmup() {
  local port=$1
  echo "WARMUP (unrecorded): ab -n 500 -c 8 -k on :$port"
  ${AB} -n 500 -c 8 -k "http://127.0.0.1:${port}/health" >/dev/null 2>&1 || true
}

run_ab_cell() {
  local server_label=$1
  local port=$2
  local ep=$3
  local conc=$4
  local url="http://127.0.0.1:${port}${ep}"
  echo ""
  echo "=== RAW AB: server=${server_label} ep=${ep} c=${conc} n=8000 -k ==="
  ${AB} -n 8000 -c ${conc} -k "${url}" 2>&1
}

seed_quack_db() {
  echo "Seeding $QDB (framework + app)..."
  rm -f "$QDB" "$QDB.wal"
  "$DUCK" "$QDB" -c ".read $FRAMEWORK" >/dev/null 2>&1
  "$DUCK" "$QDB" -c ".read $APP" >/dev/null 2>&1
  echo "Quack DB seeded."
}

seed_fast_db() {
  echo "Seeding $FDB (users table only for fastapi duck)..."
  rm -f "$FDB" "$FDB.wal"
  "$DUCK" "$FDB" -c '
    CREATE SEQUENCE IF NOT EXISTS users_id_seq START 100;
    CREATE TABLE IF NOT EXISTS users (id INTEGER DEFAULT nextval('\''users_id_seq'\''), name VARCHAR, age INTEGER);
    TRUNCATE TABLE users;
    INSERT INTO users (id, name, age) VALUES (1,'\''alice'\'',30),(2,'\''bob'\'',25),(3,'\''carol'\'',40);
  ' >/dev/null 2>&1
  echo "Fast DB seeded."
}

setup_venv() {
  if [[ ! -d "$VENV" ]]; then
    echo "Creating venv at $VENV ..."
    python3 -m venv "$VENV"
  fi
  # shellcheck disable=SC1091
  source "$VENV/bin/activate"
  pip install -q 'fastapi' 'uvicorn[standard]' 'duckdb'
  echo "venv ready: $(python -c "
import fastapi, uvicorn, duckdb
print('fastapi', fastapi.__version__, 'uvicorn ok', 'duckdb', duckdb.__version__)
" )"
}

start_quack() {
  local port=$1
  cleanup_port $port
  seed_quack_db
  echo "Starting quackapi (serve_brain) on $port ..."
  (
    printf "LOAD '%s';\nSELECT serve_brain(%s, '%s');\nSELECT block_forever(0);\n" "$EXT" "$port" "$QDB" | \
      "$DUCK" -unsigned "$QDB" > "${LOGDIR}/qbench_quack.log" 2>&1
  ) &
  wait_for_listen $port
  sleep 0.2
  local pid
  pid=$(get_listen_pid $port)
  echo "quack LISTENING pid=$pid (from lsof)"
  echo "QPID=$pid" > "${LOGDIR}/qbench_quack.pid"
}

start_uvicorn() {
  local pyfile=$1
  local port=$2
  local workers=$3
  local label=$4
  cleanup_port $port
  # shellcheck disable=SC1091
  source "$VENV/bin/activate"
  echo "Starting uvicorn $label (workers=$workers) on $port ..."
  local warg="--workers $workers"
  # Use --app-dir so module can be found without PYTHONPATH hacks; log-level warning to reduce noise
  uvicorn --app-dir "$BENCH_DIR" \
    --host 127.0.0.1 --port "$port" $warg \
    --log-level warning \
    "${pyfile}:app" > "${LOGDIR}/qbench_${label}.log" 2>&1 &
  local upid=$!
  if [[ "$workers" != "1" ]]; then
    # multi-worker fork takes longer to bind and workers ready
    sleep 1.5
  else
    sleep 0.3
  fi
  wait_for_listen $port
  local pid
  pid=$(get_listen_pid $port)
  echo "$label LISTENING pid=$pid (uvicorn bg pid was $upid)"
  echo "UPID=$pid" > "${LOGDIR}/qbench_${label}.pid"
}

kill_server() {
  local port=$1
  local label=$2
  local pid
  pid=$(get_listen_pid $port)
  if [[ -n "$pid" ]]; then
    echo "Killing $label exact pid=$pid (lsof on $port)"
    kill "$pid" 2>/dev/null || true
    wait_port_free $port
  else
    echo "No listener found on $port for $label (already dead?)"
  fi
}

verify_bodies() {
  local port=$1
  local server_label=$2
  echo ""
  echo "=== CURL BODIES (for parity check) server=$server_label port=$port ==="
  echo "/health:"
  curl_body $port "/health"
  echo ""
  echo "/users:"
  curl_body $port "/users"
  echo ""
  echo "/users/1:"
  curl_body $port "/users/1"
  echo ""
  echo "/search?q=al&limit=5:"
  curl_body $port "/search?q=al&limit=5"
  echo ""
}

run_server_matrix() {
  local label=$1
  local port=$2
  verify_bodies $port "$label"
  run_warmup $port
  for ep in /health /users /users/1 '/search?q=al&limit=5'; do
    for c in 8 64; do
      run_ab_cell "$label" "$port" "$ep" "$c"
    done
  done
}

main() {
  echo "=== SETUP ==="
  setup_venv
  seed_fast_db   # quack seed happens inside start_quack

  # A: quackapi serve_brain (16 workers hard)
  echo ""
  echo "========== SERVER A: quackapi serve_brain (port $P_QUACK) =========="
  start_quack $P_QUACK
  run_server_matrix "A_quack" $P_QUACK
  kill_server $P_QUACK "A_quack"

  # B: fast mem, 1 worker
  echo ""
  echo "========== SERVER B: fastapi_ref_mem uvicorn workers=1 (port $P_MEM1) =========="
  start_uvicorn "fastapi_ref_mem" $P_MEM1 1 "B_mem1"
  run_server_matrix "B_mem1" $P_MEM1
  kill_server $P_MEM1 "B_mem1"

  # C: fast mem, NCPU workers
  echo ""
  echo "========== SERVER C: fastapi_ref_mem uvicorn workers=$NCPU (port $P_MEMN) =========="
  start_uvicorn "fastapi_ref_mem" $P_MEMN "$NCPU" "C_memN"
  run_server_matrix "C_memN" $P_MEMN
  kill_server $P_MEMN "C_memN"

  # D: fast duckdb, 1 worker
  echo ""
  echo "========== SERVER D: fastapi_ref_duckdb uvicorn workers=1 (port $P_DUCK1) =========="
  start_uvicorn "fastapi_ref_duckdb" $P_DUCK1 1 "D_duck1"
  run_server_matrix "D_duck1" $P_DUCK1
  kill_server $P_DUCK1 "D_duck1"

  # E: fast duckdb, NCPU workers
  echo ""
  echo "========== SERVER E: fastapi_ref_duckdb uvicorn workers=$NCPU (port $P_DUCKN) =========="
  start_uvicorn "fastapi_ref_duckdb" $P_DUCKN "$NCPU" "E_duckN"
  run_server_matrix "E_duckN" $P_DUCKN
  kill_server $P_DUCKN "E_duckN"

  echo ""
  echo "=== FINAL CLEAN (rm qbench dbs) ==="
  rm -f "$QDB" "$QDB.wal" "$FDB" "$FDB.wal" || true
  echo "dbs removed. venv left at $VENV"
  echo "Bench complete. All raw ab output is above."
}

main "$@"
