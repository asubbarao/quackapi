#!/usr/bin/env bash
# =============================================================================
# run_conformance.sh — Differential conformance suite for quackapi vs FastAPI
#
# Usage (from repo root or from test/conformance/):
#   bash test/conformance/run_conformance.sh
#
# Ports used: 18500 (quackapi), 18501 (FastAPI/uvicorn)
# NEVER touches 9494 or 9495.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

QK_PORT=18500
FA_PORT=18501
QK_URL="http://127.0.0.1:$QK_PORT"
FA_URL="http://127.0.0.1:$FA_PORT"

DUCK="${DUCKDB:-$HOME/.local/bin/duckdb}"
EXT_SRC="$REPO_ROOT/ext-cpp/build/release/extension/quackapi/quackapi.duckdb_extension"
EXT_COPY="/tmp/qext_conf/quackapi.duckdb_extension"

QK_DB="/tmp/qconf_quackapi.db"
FA_VENV="$SCRIPT_DIR/fastapi_mirror/.venv"
FA_APP="$SCRIPT_DIR/fastapi_mirror/app.py"

QK_PID=""
FA_PID=""

log() { echo "[conf] $*" >&2; }
die() { echo "[conf] FATAL: $*" >&2; exit 1; }

# ── Cleanup trap ──────────────────────────────────────────────────────────────
cleanup() {
  log "Cleaning up..."
  if [ -n "$QK_PID" ] && kill -0 "$QK_PID" 2>/dev/null; then
    log "Killing quackapi server PID $QK_PID"
    kill "$QK_PID" 2>/dev/null || true
  fi
  if [ -n "$FA_PID" ] && kill -0 "$FA_PID" 2>/dev/null; then
    log "Killing FastAPI server PID $FA_PID"
    kill "$FA_PID" 2>/dev/null || true
  fi
  rm -f "$QK_DB" "${QK_DB}.wal"
}
trap cleanup EXIT

# ── Step 1: Copy extension ────────────────────────────────────────────────────
log "Copying quackapi extension..."
if [ ! -f "$EXT_SRC" ]; then
  die "Extension not found at $EXT_SRC — build ext-cpp first"
fi
mkdir -p /tmp/qext_conf
cp "$EXT_SRC" "$EXT_COPY"
log "Extension: $EXT_COPY ($(du -sh "$EXT_COPY" | cut -f1))"

# ── Step 2: Boot quackapi ─────────────────────────────────────────────────────
log "Booting quackapi on port $QK_PORT..."
rm -f "$QK_DB" "${QK_DB}.wal"

# Initialize the DB with framework + app SQL first
"$DUCK" "$QK_DB" -c ".read $REPO_ROOT/framework.sql" > /tmp/qconf_init.log 2>&1 || {
  log "framework.sql init output:"
  cat /tmp/qconf_init.log
  die "framework.sql failed"
}
"$DUCK" "$QK_DB" -c ".read $REPO_ROOT/app.sql" >> /tmp/qconf_init.log 2>&1 || {
  log "app.sql init output:"
  cat /tmp/qconf_init.log
  die "app.sql failed"
}

# Boot server using serve_brain_ex via the extension
"$DUCK" -unsigned "$QK_DB" -c "
LOAD '$EXT_COPY';
SELECT serve_brain_ex($QK_PORT, '$QK_DB', false);
SELECT block_forever(0);
" > /tmp/qconf_quackapi.log 2>&1 &
QK_PID=$!
log "quackapi started as PID $QK_PID"

# Health check quackapi
log "Waiting for quackapi to be ready..."
for i in $(seq 1 30); do
  if curl -s --connect-timeout 2 "$QK_URL/health" >/dev/null 2>&1; then
    log "quackapi ready after ${i}s"
    break
  fi
  sleep 1
  if [ "$i" -eq 30 ]; then
    log "quackapi server log:"
    cat /tmp/qconf_quackapi.log || true
    die "quackapi did not start after 30s"
  fi
done

# Verify health response
QK_HEALTH=$(curl -s "$QK_URL/health" 2>/dev/null || echo "")
log "quackapi /health: $QK_HEALTH"

# ── Step 3: Install FastAPI deps and boot ─────────────────────────────────────
log "Setting up FastAPI virtualenv..."
if [ ! -d "$FA_VENV" ]; then
  python3 -m venv "$FA_VENV"
fi
"$FA_VENV/bin/pip" install --quiet -r "$SCRIPT_DIR/fastapi_mirror/requirements.txt"
log "FastAPI deps installed."

log "Booting FastAPI on port $FA_PORT..."
"$FA_VENV/bin/uvicorn" \
  --app-dir "$SCRIPT_DIR/fastapi_mirror" \
  app:app \
  --host 127.0.0.1 \
  --port "$FA_PORT" \
  --log-level warning \
  > /tmp/qconf_fastapi.log 2>&1 &
FA_PID=$!
log "FastAPI started as PID $FA_PID"

log "Waiting for FastAPI to be ready..."
for i in $(seq 1 20); do
  if curl -s --connect-timeout 2 "$FA_URL/health" >/dev/null 2>&1; then
    log "FastAPI ready after ${i}s"
    break
  fi
  sleep 1
  if [ "$i" -eq 20 ]; then
    log "FastAPI server log:"
    cat /tmp/qconf_fastapi.log || true
    die "FastAPI did not start after 20s"
  fi
done

FA_HEALTH=$(curl -s "$FA_URL/health" 2>/dev/null || echo "")
log "FastAPI /health: $FA_HEALTH"

# ── Step 4: Run driver ────────────────────────────────────────────────────────
log "Running conformance driver..."
cd "$SCRIPT_DIR"
python3 driver.py \
  --qk "$QK_URL" \
  --fa "$FA_URL" \
  --cases cases.jsonl \
  --out results.jsonl

# ── Step 5: Generate report ───────────────────────────────────────────────────
log "Generating CONFORMANCE_REPORT.md..."
python3 generate_report.py

log "Done. See test/conformance/CONFORMANCE_REPORT.md"
