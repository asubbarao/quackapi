#!/usr/bin/env bash
# quack_from_fastapi.sh — ONE call: FastAPI/Pydantic repo → live quackapi routes
#
# Usage:
#   quack_from_fastapi.sh /path/to/app [port] [serve|gen-only]
#
# Pipeline:
#   path → read_ast (sitting_duck) → IR routes+models → CREATE ROUTE (+ BODY SCHEMA)
#        → FIFO register into running duckdb+quackapi → quackapi_serve(port)
#
# Never uses duckdb -c for CREATE ROUTE (parser_extension one-statement gotcha).
# DDL is fed via interactive FIFO stdin.
set -euo pipefail

DUCKDB_BIN="${DUCKDB_BIN:-/Users/aloksubbarao/personal/quackapi/build/release/duckdb}"
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
CORE_SQL="${SELF_DIR}/quack_from_fastapi_core.sql"
OUT_ROOT="${QUACK_FROMFAST_OUT:-${SELF_DIR}/out}"

if [[ $# -lt 1 ]]; then
  echo "usage: $0 /path/to/fastapi-or-pydantic-repo [port] [serve|gen-only]" >&2
  exit 2
fi

APP_PATH="$1"
PORT="${2:-18826}"
MODE="${3:-serve}"

if [[ ! -d "$APP_PATH" && ! -f "$APP_PATH" ]]; then
  echo "path not found: $APP_PATH" >&2
  exit 2
fi
if [[ ! -x "$DUCKDB_BIN" ]]; then
  echo "duckdb binary not found: $DUCKDB_BIN" >&2
  exit 2
fi
if [[ ! -f "$CORE_SQL" ]]; then
  echo "missing core SQL: $CORE_SQL" >&2
  exit 2
fi

# Resolve absolute path + choose AST glob
APP_ROOT="$(cd "$(dirname "$APP_PATH")" && pwd)/$(basename "$APP_PATH")"
if [[ -d "$APP_ROOT/app" && -f "$APP_ROOT/app/main.py" ]]; then
  APP_GLOB="${APP_ROOT}/app/**/*.py"
elif [[ -d "$APP_ROOT/src" ]]; then
  APP_GLOB="${APP_ROOT}/src/**/*.py"
elif [[ -d "$APP_ROOT" ]]; then
  APP_GLOB="${APP_ROOT}/**/*.py"
else
  APP_GLOB="$APP_ROOT"
fi

REPO="$(basename "$APP_ROOT")"
OUT_DIR="${OUT_ROOT}/${REPO}"
mkdir -p "$OUT_DIR" "${SELF_DIR}/work"
WORK="${SELF_DIR}/work/${REPO}_$$"
mkdir -p "$WORK"

log() { echo "$@" | tee -a "$OUT_DIR/transcript.txt"; }
: >"$OUT_DIR/transcript.txt"

log "=== quack_from_fastapi ==="
log "repo:     $REPO"
log "app_root: $APP_ROOT"
log "app_glob: $APP_GLOB"
log "port:     $PORT"
log "mode:     $MODE"
log "binary:   $DUCKDB_BIN"
log "out:      $OUT_DIR"
log ""

# ── 1. Substitute placeholders into core SQL ───────────────────────────────
# Escape single quotes in paths for SQL string literals (paths themselves).
esc_sql() { printf '%s' "$1" | sed "s/'/''/g"; }

RUN_SQL="${WORK}/run_core.sql"
sed \
  -e "s|{{APP_GLOB}}|$(esc_sql "$APP_GLOB")|g" \
  -e "s|{{APP_ROOT}}|$(esc_sql "$APP_ROOT")|g" \
  -e "s|{{REPO}}|$(esc_sql "$REPO")|g" \
  -e "s|{{OUT_DIR}}|$(esc_sql "$OUT_DIR")|g" \
  "$CORE_SQL" >"$RUN_SQL"

log "--- phase 1: AST → IR → CREATE ROUTE generation ---"
set +e
"$DUCKDB_BIN" -unsigned <"$RUN_SQL" >"$OUT_DIR/gen.log" 2>&1
GEN_RC=$?
set -e
if [[ $GEN_RC -ne 0 ]]; then
  log "FAIL generation rc=$GEN_RC — see $OUT_DIR/gen.log"
  tail -40 "$OUT_DIR/gen.log" | tee -a "$OUT_DIR/transcript.txt"
  exit $GEN_RC
fi
# last summary lines
tail -30 "$OUT_DIR/gen.log" | tee -a "$OUT_DIR/transcript.txt"
log ""

# ── 2. Decode base64 CREATE ROUTE statements ────────────────────────────────
ROUTES_SQL="${OUT_DIR}/routes.sql"
python3 - <<PY
import base64, pathlib
b64 = pathlib.Path("${OUT_DIR}/routes.sql.b64")
out = pathlib.Path("${ROUTES_SQL}")
stmts = []
if b64.exists():
    for line in b64.read_text(encoding="utf-8").splitlines():
        line = line.strip().strip('"')
        if not line:
            continue
        stmts.append(base64.b64decode(line).decode("utf-8"))
out.write_text("\n\n".join(stmts) + ("\n" if stmts else ""), encoding="utf-8")
print(f"emitted {len(stmts)} CREATE ROUTE statement(s) → {out}")
PY
log "routes.sql: $(wc -l <"$ROUTES_SQL" | tr -d ' ') lines, $(grep -c 'CREATE OR REPLACE ROUTE' "$ROUTES_SQL" || echo 0) statements"
log ""

if [[ "$MODE" == "gen-only" ]]; then
  log "gen-only: skipping serve"
  cat "$OUT_DIR/summary.json" 2>/dev/null || true
  exit 0
fi

# ── 3. FIFO boot + register + serve ─────────────────────────────────────────
# macOS mktemp requires trailing X's — do NOT put extensions after XXXXXX.
FIFO="${WORK}/cmd.fifo"
LOG="${WORK}/duckdb.log"
rm -f "$FIFO"
mkfifo "$FIFO"

# free port
stale="$(lsof -nP -iTCP:"$PORT" -sTCP:LISTEN -t 2>/dev/null || true)"
if [[ -n "$stale" ]]; then
  kill $stale 2>/dev/null || true
  sleep 0.3
fi

log "--- phase 2: FIFO register + quackapi_serve($PORT) ---"
"$DUCKDB_BIN" -unsigned <"$FIFO" >"$LOG" 2>&1 &
DPID=$!
exec 3>"$FIFO"

{
  echo "LOAD quackapi;"
  # Feed CREATE ROUTE one statement at a time (parser_extension gotcha).
  # Split on ';\n' boundaries that end CREATE statements.
  python3 - <<'PY' "$ROUTES_SQL"
import sys, re
text = open(sys.argv[1], encoding="utf-8").read()
# split on semicolon at end of statement
parts = re.split(r';\s*\n', text)
for p in parts:
    s = p.strip()
    if not s:
        continue
    if not s.endswith(';'):
        s += ';'
    print(s)
    print()
PY
  echo "SELECT count(*) AS n_routes FROM quackapi_routes();"
  echo "SELECT * FROM quackapi_routes() LIMIT 50;"
  echo "SELECT * FROM quackapi_serve(${PORT}, host := '127.0.0.1');"
} >&3

# wait for listen
ok=0
for i in $(seq 1 80); do
  if curl -sS -o /dev/null --connect-timeout 0.2 "http://127.0.0.1:${PORT}/_qf/health" 2>/dev/null; then
    ok=1
    break
  fi
  if ! kill -0 "$DPID" 2>/dev/null; then
    log "duckdb died early — log:"
    tail -50 "$LOG" | tee -a "$OUT_DIR/transcript.txt"
    exec 3>&- || true
    exit 1
  fi
  sleep 0.15
done

if [[ $ok -ne 1 ]]; then
  log "FAIL: server not listening on $PORT"
  tail -50 "$LOG" | tee -a "$OUT_DIR/transcript.txt"
  exec 3>&- || true
  kill "$DPID" 2>/dev/null || true
  exit 1
fi

log "server up pid=$DPID port=$PORT"
# capture route registry from server log (SELECT printed before serve blocks)
grep -E 'n_routes|handler|path|METHOD|method' "$LOG" | head -60 | tee -a "$OUT_DIR/transcript.txt" || true

# expose state for prove scripts / parent shell
echo "$DPID" >"$OUT_DIR/server.pid"
echo "$PORT" >"$OUT_DIR/server.port"
echo "$LOG" >"$OUT_DIR/server.logpath"
# keep FD 3 open so duckdb stdin stays alive — write pid file and exit leaving process
# Caller/prove script will curl; we stay attached if interactive.
# For one-shot: keep backgrounded with open writer via a side process.

# Hold FIFO open in background so serve stays alive after this script returns optionally.
# When MODE=serve (default), leave a holder and print how to stop.
# Keep FIFO writer open so duckdb interactive session (and quackapi_serve) stay alive.
# macOS sleep has no `infinity` — use a long finite sleep.
(
  exec 3>"$FIFO"
  while kill -0 "$DPID" 2>/dev/null; do
    sleep 3600
  done
) &
HOLD_PID=$!
echo "$HOLD_PID" >"$OUT_DIR/hold.pid"
# close our FD 3 after holder has the write end
exec 3>&-

log ""
log "READY: quack_from_fastapi('$APP_ROOT') registered on :$PORT"
log "health: curl -s http://127.0.0.1:${PORT}/_qf/health"
log "stop:   kill \$(cat $OUT_DIR/server.pid) \$(cat $OUT_DIR/hold.pid 2>/dev/null)"
log ""
# dump summary
if [[ -f "$OUT_DIR/summary.json" ]]; then
  log "summary:"
  cat "$OUT_DIR/summary.json" | tee -a "$OUT_DIR/transcript.txt"
fi
