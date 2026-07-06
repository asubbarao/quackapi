#!/usr/bin/env bash
# =============================================================================
# run_conformance_pure.sh — Pure-SQL conformance harness for quackapi
#
# No C extension required. Tests the handle_request() SQL macro directly
# via the DuckDB CLI against FastAPI responses from a live uvicorn instance.
#
# Usage:
#   bash test/conformance/run_conformance_pure.sh
#
# FastAPI runs on port 18351 (well away from 9494/9495 and the C-extension
# harness port 18500/18501).
#
# NEVER touches ports 9494 or 9495.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

FA_PORT=18351
FA_URL="http://127.0.0.1:$FA_PORT"

FA_VENV="$SCRIPT_DIR/fastapi_mirror/.venv"
FA_APP_DIR="$SCRIPT_DIR/fastapi_mirror"

FA_PID=""

log() { echo "[conf-pure] $*" >&2; }
die() { echo "[conf-pure] FATAL: $*" >&2; exit 1; }

# ── Cleanup trap ──────────────────────────────────────────────────────────────
cleanup() {
  log "Cleaning up..."
  if [ -n "$FA_PID" ] && kill -0 "$FA_PID" 2>/dev/null; then
    log "Killing FastAPI server PID $FA_PID"
    kill "$FA_PID" 2>/dev/null || true
  fi
  # Kill any leftover on the port just in case
  local stale
  stale=$(lsof -nP -iTCP:$FA_PORT -sTCP:LISTEN -t 2>/dev/null || true)
  if [ -n "$stale" ] && [ "$stale" != "$FA_PID" ]; then
    log "Killing stale PID $stale on port $FA_PORT"
    kill "$stale" 2>/dev/null || true
  fi
}
trap cleanup EXIT

# ── Verify DuckDB ─────────────────────────────────────────────────────────────
DUCK="${DUCKDB:-/opt/homebrew/bin/duckdb}"
if [ ! -x "$DUCK" ]; then
  die "DuckDB not found at $DUCK — set DUCKDB env var"
fi
log "DuckDB: $($DUCK --version 2>/dev/null)"

# ── Verify framework.sql + app.sql exist ──────────────────────────────────────
[ -f "$REPO_ROOT/framework.sql" ] || die "framework.sql not found at $REPO_ROOT"
[ -f "$REPO_ROOT/app.sql" ] || die "app.sql not found at $REPO_ROOT"
log "framework.sql and app.sql found at $REPO_ROOT"

# ── Quick handle_request smoke test ───────────────────────────────────────────
log "Smoke-testing handle_request() via DuckDB CLI..."
SMOKE=$(printf '.read %s\n.read %s\nSELECT status_code FROM handle_request('"'"'GET'"'"','"'"'/health'"'"','"'"'{}'"'"','"'"''"'"');\n' \
  "$REPO_ROOT/framework.sql" "$REPO_ROOT/app.sql" | \
  "$DUCK" -json :memory: 2>/dev/null | tail -1 || echo "")
if echo "$SMOKE" | grep -q '"status_code":200'; then
  log "Smoke test PASSED: handle_request returns 200 for /health"
else
  log "Smoke test output: $SMOKE"
  die "Smoke test FAILED — handle_request did not return 200 for /health"
fi

# ── Kill any existing listener on FA_PORT ─────────────────────────────────────
STALE=$(lsof -nP -iTCP:$FA_PORT -sTCP:LISTEN -t 2>/dev/null || true)
if [ -n "$STALE" ]; then
  log "Port $FA_PORT already in use by PID $STALE — killing it"
  kill "$STALE" 2>/dev/null || true
  sleep 1
fi

# ── Setup FastAPI venv ────────────────────────────────────────────────────────
log "Setting up FastAPI virtualenv at $FA_VENV..."
if [ ! -d "$FA_VENV" ]; then
  python3 -m venv "$FA_VENV"
  log "Created new venv"
fi
"$FA_VENV/bin/pip" install --quiet -r "$FA_APP_DIR/requirements.txt"
log "FastAPI deps installed (fastapi, uvicorn[standard], python-multipart)"

# ── Boot FastAPI ──────────────────────────────────────────────────────────────
log "Booting FastAPI reference on port $FA_PORT..."
"$FA_VENV/bin/uvicorn" \
  --app-dir "$FA_APP_DIR" \
  app:app \
  --host 127.0.0.1 \
  --port "$FA_PORT" \
  --log-level warning \
  > /tmp/qconf_pure_fastapi.log 2>&1 &
FA_PID=$!
log "FastAPI started as PID $FA_PID"

# Wait for FastAPI to be ready
log "Waiting for FastAPI to be ready..."
for i in $(seq 1 25); do
  if curl -sf --connect-timeout 2 "$FA_URL/health" >/dev/null 2>&1; then
    log "FastAPI ready after ${i}s"
    break
  fi
  sleep 1
  if [ "$i" -eq 25 ]; then
    log "FastAPI server log:"
    cat /tmp/qconf_pure_fastapi.log || true
    die "FastAPI did not start after 25s"
  fi
done

FA_HEALTH=$(curl -s "$FA_URL/health" 2>/dev/null || echo "")
log "FastAPI /health: $FA_HEALTH"

# ── Run driver ────────────────────────────────────────────────────────────────
log "Running pure-SQL conformance driver..."
cd "$SCRIPT_DIR"

python3 driver_pure.py \
  --fa "$FA_URL" \
  --cases cases.jsonl \
  --out results_pure.jsonl \
  --verbose

log "Results written to $SCRIPT_DIR/results_pure.jsonl"
log "Done."
