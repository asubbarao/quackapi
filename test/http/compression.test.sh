#!/usr/bin/env bash
# HTTP integration: Accept-Encoding content negotiation (zstd default, gzip, identity)
# + min-size threshold. Compressed bodies must decompress to the original JSON.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18971}"
INIT="$(mktemp /tmp/quackapi_compression_XXXXXX.sql)"

# Payload large enough to exceed default min (256) and actually shrink under zstd/gzip.
# ~800 bytes of repeated JSON-friendly text.
cat >"$INIT" <<'SQL'
CREATE ROUTE bulk GET '/bulk' AS
  SELECT repeat('abcdefghijklmnopqrstuvwxyz0123456789-', 40) AS payload;

CREATE ROUTE tiny GET '/tiny' AS
  SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

# Fetch identity baseline (curl may send Accept-Encoding by default — force identity)
BASE_HDR="$(mktemp)"
BASE_BODY="$(mktemp)"
curl -sS -D "$BASE_HDR" -o "$BASE_BODY" \
  -H 'Accept-Encoding: identity' \
  "http://127.0.0.1:${PORT}/bulk"
BASE_JSON="$(cat "$BASE_BODY")"
if [[ -z "$BASE_JSON" ]]; then
  echo "ASSERT FAIL: empty baseline body" >&2
  exit 1
fi
# Expect uncompressed identity (no Content-Encoding)
if grep -qi '^Content-Encoding:' "$BASE_HDR"; then
  echo "ASSERT FAIL: identity request got Content-Encoding" >&2
  cat "$BASE_HDR" >&2
  exit 1
fi
echo "-- baseline identity body bytes: $(wc -c <"$BASE_BODY")"

echo "-- 1. Accept-Encoding: zstd → Content-Encoding: zstd + roundtrip"
ZSTD_HDR="$(mktemp)"
ZSTD_BODY="$(mktemp)"
curl -sS -D "$ZSTD_HDR" -o "$ZSTD_BODY" \
  -H 'Accept-Encoding: zstd' \
  --compressed \
  "http://127.0.0.1:${PORT}/bulk" 2>/dev/null || true
# curl --compressed may not know zstd on all builds; fetch raw and decompress ourselves
curl -sS -D "$ZSTD_HDR" -o "$ZSTD_BODY" \
  -H 'Accept-Encoding: zstd' \
  "http://127.0.0.1:${PORT}/bulk"
if ! grep -qi '^Content-Encoding:[[:space:]]*zstd' "$ZSTD_HDR"; then
  echo "ASSERT FAIL: expected Content-Encoding: zstd" >&2
  cat "$ZSTD_HDR" >&2
  exit 1
fi
if ! grep -qi '^Vary:.*Accept-Encoding' "$ZSTD_HDR"; then
  # Vary may be on its own line (httplib multimap can emit separate Vary headers)
  if ! grep -qi '^Vary:[[:space:]]*Accept-Encoding' "$ZSTD_HDR"; then
    echo "ASSERT FAIL: missing Vary: Accept-Encoding" >&2
    cat "$ZSTD_HDR" >&2
    exit 1
  fi
fi
ZSTD_ROUND="$(mktemp)"
zstd -d -c -q "$ZSTD_BODY" >"$ZSTD_ROUND"
if ! cmp -s "$BASE_BODY" "$ZSTD_ROUND"; then
  echo "ASSERT FAIL: zstd roundtrip != original JSON" >&2
  echo "  original: $(head -c 120 "$BASE_BODY")..." >&2
  echo "  decoded:  $(head -c 120 "$ZSTD_ROUND")..." >&2
  exit 1
fi
echo "  zstd ok: $(wc -c <"$ZSTD_BODY") compressed → $(wc -c <"$ZSTD_ROUND") original"

echo "-- 2. Accept-Encoding: gzip → Content-Encoding: gzip + roundtrip"
GZIP_HDR="$(mktemp)"
GZIP_BODY="$(mktemp)"
curl -sS -D "$GZIP_HDR" -o "$GZIP_BODY" \
  -H 'Accept-Encoding: gzip' \
  "http://127.0.0.1:${PORT}/bulk"
if ! grep -qi '^Content-Encoding:[[:space:]]*gzip' "$GZIP_HDR"; then
  echo "ASSERT FAIL: expected Content-Encoding: gzip" >&2
  cat "$GZIP_HDR" >&2
  exit 1
fi
GZIP_ROUND="$(mktemp)"
gzip -dc <"$GZIP_BODY" >"$GZIP_ROUND"
if ! cmp -s "$BASE_BODY" "$GZIP_ROUND"; then
  echo "ASSERT FAIL: gzip roundtrip != original JSON" >&2
  exit 1
fi
echo "  gzip ok: $(wc -c <"$GZIP_BODY") compressed → $(wc -c <"$GZIP_ROUND") original"

echo "-- 3. Accept-Encoding: gzip, zstd → prefer zstd (owner default)"
BOTH_HDR="$(mktemp)"
BOTH_BODY="$(mktemp)"
curl -sS -D "$BOTH_HDR" -o "$BOTH_BODY" \
  -H 'Accept-Encoding: gzip, zstd' \
  "http://127.0.0.1:${PORT}/bulk"
if ! grep -qi '^Content-Encoding:[[:space:]]*zstd' "$BOTH_HDR"; then
  echo "ASSERT FAIL: gzip+zstd should prefer zstd" >&2
  cat "$BOTH_HDR" >&2
  exit 1
fi

echo "-- 4. no Accept-Encoding → identity (no Content-Encoding)"
# curl may inject Accept-Encoding by default; an empty header suppresses it (curl 8.x).
NO_AE_HDR="$(mktemp)"
NO_AE_BODY="$(mktemp)"
curl -sS -D "$NO_AE_HDR" -o "$NO_AE_BODY" \
  -H 'Accept-Encoding:' \
  "http://127.0.0.1:${PORT}/bulk"
if grep -qi '^Content-Encoding:' "$NO_AE_HDR"; then
  echo "ASSERT FAIL: request without Accept-Encoding got Content-Encoding" >&2
  cat "$NO_AE_HDR" >&2
  exit 1
fi
if ! cmp -s "$BASE_BODY" "$NO_AE_BODY"; then
  echo "ASSERT FAIL: no-AE body differs from identity baseline" >&2
  exit 1
fi

echo "-- 5. tiny body under threshold → uncompressed even with Accept-Encoding: zstd"
TINY_HDR="$(mktemp)"
TINY_BODY="$(mktemp)"
curl -sS -D "$TINY_HDR" -o "$TINY_BODY" \
  -H 'Accept-Encoding: zstd, gzip' \
  "http://127.0.0.1:${PORT}/tiny"
if grep -qi '^Content-Encoding:' "$TINY_HDR"; then
  echo "ASSERT FAIL: tiny body should not be compressed" >&2
  cat "$TINY_HDR" >&2
  echo "  body: $(cat "$TINY_BODY")" >&2
  exit 1
fi
assert_body_contains "$(cat "$TINY_BODY")" '"status"' "tiny_json"

echo "-- 6. compression := false opt-out"
stop_quackapi
INIT2="$(mktemp /tmp/quackapi_compression_off_XXXXXX.sql)"
cat >"$INIT2" <<'SQL'
CREATE ROUTE bulk GET '/bulk' AS
  SELECT repeat('abcdefghijklmnopqrstuvwxyz0123456789-', 40) AS payload;
SQL
# boot with compression off via SET before serve
_QA_PORT="$PORT"
_QA_FIFO="$(mktemp -u /tmp/quackapi_http_XXXXXX.fifo)"
_QA_LOG="$(mktemp /tmp/quackapi_http_XXXXXX.log)"
rm -f "$_QA_FIFO"
mkfifo "$_QA_FIFO"
stale="$(lsof -nP -iTCP:"$PORT" -sTCP:LISTEN -t 2>/dev/null || true)"
if [[ -n "$stale" ]]; then kill $stale 2>/dev/null || true; sleep 0.2; fi
"$DUCKDB_BIN" -unsigned <"$_QA_FIFO" >"$_QA_LOG" 2>&1 &
_QA_PID=$!
exec 3>"$_QA_FIFO"
_QA_FD=3
{
  echo "LOAD quackapi;"
  cat "$INIT2"
  echo
  echo "SET quackapi_compression = false;"
  echo "SELECT * FROM quackapi_serve(${PORT});"
} >&3
rm -f "$INIT2"
for i in $(seq 1 80); do
  if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then break; fi
  if ! kill -0 "$_QA_PID" 2>/dev/null; then
    echo "duckdb exited early; log:" >&2
    cat "$_QA_LOG" >&2
    exit 3
  fi
  sleep 0.1
done
OFF_HDR="$(mktemp)"
OFF_BODY="$(mktemp)"
curl -sS -D "$OFF_HDR" -o "$OFF_BODY" \
  -H 'Accept-Encoding: zstd, gzip' \
  "http://127.0.0.1:${PORT}/bulk"
if grep -qi '^Content-Encoding:' "$OFF_HDR"; then
  echo "ASSERT FAIL: compression=false still set Content-Encoding" >&2
  cat "$OFF_HDR" >&2
  exit 1
fi
if ! cmp -s "$BASE_BODY" "$OFF_BODY"; then
  echo "ASSERT FAIL: opt-out body differs from identity baseline" >&2
  exit 1
fi

rm -f "$BASE_HDR" "$BASE_BODY" "$ZSTD_HDR" "$ZSTD_BODY" "$ZSTD_ROUND" \
  "$GZIP_HDR" "$GZIP_BODY" "$GZIP_ROUND" "$BOTH_HDR" "$BOTH_BODY" \
  "$NO_AE_HDR" "$NO_AE_BODY" "$TINY_HDR" "$TINY_BODY" "$OFF_HDR" "$OFF_BODY"

echo "compression.test.sh OK"
stop_quackapi
