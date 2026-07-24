#!/usr/bin/env bash
# Boot stack A (quackapi). Each process is a full DuckDB: routes + Postgres ATTACH.
# WORKERS=1 (default) → one process on PORT.
# WORKERS=N → N processes on PORT+1..PORT+N, thin round-robin proxy on PORT
#   (same shape as uvicorn --workers N: multi-process, shared backend).
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-/Users/aloksubbarao/personal/quackapi/build/release/duckdb}"
EXT="${QUACKAPI_EXT:-/Users/aloksubbarao/personal/quackapi/build/release/extension/quackapi/quackapi.duckdb_extension}"
ROUTES="${BENCH_DIR}/routes.sql"
HOST="${QUACKAPI_HOST:-127.0.0.1}"
PORT="${QUACKAPI_PORT:-8000}"
WORKERS="${WORKERS:-1}"

if [[ ! -x "$DUCKDB_BIN" ]]; then
  echo "serve_quackapi: duckdb binary not found: $DUCKDB_BIN" >&2
  exit 1
fi
if [[ ! -f "$EXT" ]]; then
  echo "serve_quackapi: extension not found: $EXT" >&2
  exit 1
fi
if [[ ! -f "$ROUTES" ]]; then
  echo "serve_quackapi: routes file not found: $ROUTES" >&2
  exit 1
fi

boot_one() {
  local port="$1"
  # Cap PG pool per process so WORKERS×pool stays under Postgres max_connections.
  # force-mode can open past the cap under parallel scans — keep pool tiny + threads low
  # when multi-process. Single process keeps a larger pool.
  local pool=32
  local duck_threads=4
  local httpt=32
  if [[ "$WORKERS" -gt 1 ]]; then
    pool=4
    duck_threads=1
    httpt=8
  fi
  {
    printf "LOAD '%s';\n" "$EXT"
    sed -e "s/SET pg_pool_max_connections = 64;/SET pg_pool_max_connections = ${pool};\nSET pg_pool_acquire_mode = 'wait';\nSET threads = ${duck_threads};/" \
        "$ROUTES"
    # pg_dsn → libpq path (FastAPI-shaped). Same DSN as ATTACH.
    printf "SELECT * FROM quackapi_serve(%s, host := '%s', access_log := false, enable_logging := false, worker_threads := %s, pg_dsn := 'postgresql://admin:password@127.0.0.1:6432/quackbench');\n" \
      "$port" "$HOST" "$httpt"
    while :; do sleep 86400; done
  } | exec "$DUCKDB_BIN" -unsigned -init /dev/null :memory:
}

if [[ "$WORKERS" -le 1 ]]; then
  boot_one "$PORT"
  exit 0
fi

# Multi-process: workers on PORT+1 .. PORT+WORKERS, proxy on PORT.
PIDS=()
cleanup() {
  local p
  for p in "${PIDS[@]:-}"; do
    kill "$p" 2>/dev/null || true
    pkill -P "$p" 2>/dev/null || true
  done
}
trap cleanup EXIT INT TERM

i=1
while [[ $i -le $WORKERS ]]; do
  wport=$((PORT + i))
  boot_one "$wport" &
  PIDS+=($!)
  i=$((i + 1))
done

# Wait until every worker answers /hello.
ready=0
for _ in $(seq 1 120); do
  ready=1
  i=1
  while [[ $i -le $WORKERS ]]; do
    if ! curl -sf --max-time 1 "http://127.0.0.1:$((PORT + i))/hello" >/dev/null 2>&1; then
      ready=0
      break
    fi
    i=$((i + 1))
  done
  [[ $ready -eq 1 ]] && break
  sleep 0.25
done
if [[ $ready -ne 1 ]]; then
  echo "serve_quackapi: workers not ready on $((PORT + 1))..$((PORT + WORKERS))" >&2
  exit 1
fi

# Round-robin reverse proxy (stdlib only). Front door = PORT.
export QUACK_LB_PORT="$PORT"
export QUACK_LB_WORKERS="$WORKERS"
exec python3 - <<'PY'
import os, socket, threading, itertools, select, sys

port = int(os.environ["QUACK_LB_PORT"])
n = int(os.environ["QUACK_LB_WORKERS"])
backends = itertools.cycle([("127.0.0.1", port + i) for i in range(1, n + 1)])
lock = threading.Lock()

def next_backend():
    with lock:
        return next(backends)

def pipe(a, b):
    try:
        while True:
            r, _, _ = select.select([a], [], [], 60)
            if not r:
                break
            data = a.recv(65536)
            if not data:
                break
            b.sendall(data)
    except Exception:
        pass
    finally:
        try:
            a.shutdown(socket.SHUT_RD)
        except Exception:
            pass
        try:
            b.shutdown(socket.SHUT_WR)
        except Exception:
            pass

def handle(client):
    upstream = None
    try:
        host, bport = next_backend()
        upstream = socket.create_connection((host, bport), timeout=30)
        t1 = threading.Thread(target=pipe, args=(client, upstream), daemon=True)
        t2 = threading.Thread(target=pipe, args=(upstream, client), daemon=True)
        t1.start(); t2.start()
        t1.join(); t2.join()
    except Exception as e:
        try:
            client.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")
        except Exception:
            pass
        print(f"lb: {e}", file=sys.stderr)
    finally:
        try:
            client.close()
        except Exception:
            pass
        if upstream:
            try:
                upstream.close()
            except Exception:
                pass

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(512)
print(f"serve_quackapi: proxy :{port} -> {n} workers on :{port+1}..:{port+n}", flush=True)
while True:
    c, _ = srv.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
PY
