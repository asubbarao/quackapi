#!/usr/bin/env bash
# Verify one explicitly-built extension in an isolated DuckDB process.
set -euo pipefail

DUCKDB=""
EXTENSION=""
CURL_HTTPFS=""
HTTPFS_TIMEOUT_RETRY=""
PORT="${PORT:-}"
TMPDIR_PATH=""
DUCKDB_PID=""

usage() {
  cat >&2 <<'EOF'
Usage: scripts/system_runtime_smoke.sh --duckdb PATH --extension PATH \
  --curl-httpfs PATH --httpfs-timeout-retry PATH [--port PORT]
EOF
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --duckdb) DUCKDB="${2:-}"; shift 2 ;;
    --extension) EXTENSION="${2:-}"; shift 2 ;;
    --curl-httpfs) CURL_HTTPFS="${2:-}"; shift 2 ;;
    --httpfs-timeout-retry) HTTPFS_TIMEOUT_RETRY="${2:-}"; shift 2 ;;
    --port) PORT="${2:-}"; shift 2 ;;
    *) usage ;;
  esac
done

[[ -n "$DUCKDB" && -x "$DUCKDB" ]] || { echo "DuckDB binary is required and must be executable" >&2; usage; }
for artifact in "$EXTENSION" "$CURL_HTTPFS" "$HTTPFS_TIMEOUT_RETRY"; do
  [[ -n "$artifact" && -f "$artifact" ]] || { echo "missing extension artifact: $artifact" >&2; usage; }
done

if [[ -z "$PORT" ]]; then
  for _ in $(seq 1 20); do
    candidate=$((49152 + RANDOM % 16384))
    if ! lsof -nP -iTCP:"$candidate" -sTCP:LISTEN -t >/dev/null 2>&1; then
      PORT="$candidate"
      break
    fi
  done
fi
[[ "$PORT" =~ ^[0-9]+$ ]] || { echo "port must be numeric" >&2; exit 2; }
if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN -t >/dev/null 2>&1; then
  echo "port $PORT is already occupied" >&2
  exit 2
fi

TMPDIR_PATH="$(mktemp -d "${TMPDIR:-/tmp}/quackapi-system-runtime.XXXXXX")"
LOG="$TMPDIR_PATH/duckdb.log"
BASE="http://127.0.0.1:$PORT"

cleanup() {
  if [[ -n "$DUCKDB_PID" ]] && kill -0 "$DUCKDB_PID" 2>/dev/null; then
    kill "$DUCKDB_PID" 2>/dev/null || true
    wait "$DUCKDB_PID" 2>/dev/null || true
  fi
  [[ -n "$TMPDIR_PATH" ]] && rm -rf "$TMPDIR_PATH"
}
trap cleanup EXIT INT TERM

sql_literal() {
  local value="$1"
  printf "'%s'" "${value//\'/\'\'}"
}

load_sql="LOAD $(sql_literal "$CURL_HTTPFS"); LOAD $(sql_literal "$HTTPFS_TIMEOUT_RETRY"); LOAD $(sql_literal "$EXTENSION");"
serve_sql="
SET memory_limit = '20GiB';
SET threads = 12;
SET max_temp_directory_size = '128GiB';
SET enable_logging = false;
CREATE SCHEMA workspace;
CREATE TABLE workspace.marker AS SELECT 'system-quack-smoke' AS marker;
CREATE ROUTE runtime GET '/runtime' AS
SELECT version() AS version,
       current_setting('memory_limit') AS memory_limit,
       current_setting('threads') AS threads,
       current_setting('preserve_insertion_order') AS preserve_insertion_order,
       current_setting('enable_logging') AS enable_logging,
       (SELECT marker FROM workspace.marker) AS marker,
       (SELECT loaded FROM duckdb_extensions() WHERE extension_name = 'curl_httpfs') AS curl_httpfs_loaded,
       (SELECT loaded FROM duckdb_extensions() WHERE extension_name = 'httpfs_timeout_retry') AS httpfs_timeout_retry_loaded,
       (SELECT loaded FROM duckdb_extensions() WHERE extension_name = 'otlp') AS otlp_loaded,
       worker_threads, max_pending_requests
FROM quackapi_servers();
CREATE ROUTE capped GET '/capped' AS SELECT repeat('x', 9000000) AS text;
SELECT * FROM quackapi_serve($PORT, host := '127.0.0.1',
    worker_threads := 8, max_pending_requests := 32,
    query_timeout_ms := 30000, max_response_bytes := 8388608,
    tune := false, wire_quack_auth := false, access_log := false,
    block := true);
"

"$DUCKDB" -init /dev/null -unsigned :memory: -bail -json -cmd "$load_sql" -c "$serve_sql" >"$LOG" 2>&1 &
DUCKDB_PID=$!

for _ in $(seq 1 200); do
  if curl -fsS --connect-timeout 1 --max-time 1 "$BASE/health" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "$DUCKDB_PID" 2>/dev/null; then
    echo "DuckDB exited before the listener became ready:" >&2
    cat "$LOG" >&2
    exit 3
  fi
  sleep 0.1
done
curl -fsS --connect-timeout 1 --max-time 1 "$BASE/health" >/dev/null || {
  echo "listener did not become ready at $BASE:" >&2
  cat "$LOG" >&2
  exit 3
}

assert_runtime() {
  jq -e '
    type == "array" and length == 1 and .[0] == {
      version: "v1.5.5",
      memory_limit: "20.0 GiB",
      threads: 12,
      preserve_insertion_order: true,
      enable_logging: false,
      marker: "system-quack-smoke",
      curl_httpfs_loaded: true,
      httpfs_timeout_retry_loaded: true,
      otlp_loaded: false,
      worker_threads: 8,
      max_pending_requests: 32
    }
  ' "$1" >/dev/null
}

runtime="$TMPDIR_PATH/runtime.json"
[[ "$(curl -sS --connect-timeout 1 --max-time 5 -o "$runtime" -w '%{http_code}' "$BASE/runtime")" == 200 ]]
assert_runtime "$runtime"

pids=()
for request in $(seq 1 16); do
  response="$TMPDIR_PATH/runtime-$request.json"
  (
    status="$(curl -sS --connect-timeout 1 --max-time 5 -o "$response" -w '%{http_code}' "$BASE/runtime")"
    [[ "$status" == 200 ]]
    assert_runtime "$response"
  ) &
  pids+=("$!")
done
for pid in "${pids[@]}"; do
  wait "$pid"
done

cap="$TMPDIR_PATH/capped.json"
[[ "$(curl -sS --connect-timeout 1 --max-time 5 -o "$cap" -w '%{http_code}' "$BASE/capped")" == 507 ]]
[[ "$(curl -sS --connect-timeout 1 --max-time 5 -o "$runtime" -w '%{http_code}' "$BASE/runtime")" == 200 ]]
assert_runtime "$runtime"

jq -n \
  --arg engine "$DUCKDB" \
  --arg extension "$EXTENSION" \
  --arg extension_sha256 "$(shasum -a 256 "$EXTENSION" | awk '{print $1}')" \
  --arg curl_httpfs_sha256 "$(shasum -a 256 "$CURL_HTTPFS" | awk '{print $1}')" \
  --arg httpfs_timeout_retry_sha256 "$(shasum -a 256 "$HTTPFS_TIMEOUT_RETRY" | awk '{print $1}')" \
  --arg base "$BASE" \
  '{engine: $engine, extension: $extension, extension_sha256: $extension_sha256,
    curl_httpfs_sha256: $curl_httpfs_sha256,
    httpfs_timeout_retry_sha256: $httpfs_timeout_retry_sha256, base: $base,
    concurrent_requests: 16, cap_status: 507, recovered_after_cap: true}'
