#!/usr/bin/env bash
# CREATE STREAM SSE — curl receives text/event-stream with >=2 data: events.
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18765}"
INIT="$(mktemp /tmp/quackapi_stream_init_XXXXXX.sql)"
cat >"$INIT" <<'SQL'
CREATE STREAM ticks GET '/ticks' AS
  SELECT i AS id, 'tick' AS msg FROM range(3) t(i);
CREATE STREAM once GET '/once' AS
  SELECT 1 AS id, 'a' AS msg
  UNION ALL
  SELECT 2 AS id, 'b' AS msg;
SQL

boot_quackapi "$PORT" "$INIT"

echo "== SSE /ticks: content-type + >=2 data events =="
HDR="$(mktemp)"
BODY="$(mktemp)"
# -N: no buffer; finite stream closes after 3 events
curl -sS -N -D "$HDR" -o "$BODY" --max-time 5 "http://127.0.0.1:${PORT}/ticks"
CT="$(grep -i '^Content-Type:' "$HDR" | head -1 | tr -d '\r')"
echo "headers Content-Type: $CT"
echo "body:"
cat "$BODY"
echo

if ! echo "$CT" | grep -qi 'text/event-stream'; then
  echo "ASSERT FAIL: Content-Type not text/event-stream: $CT" >&2
  exit 1
fi

DATA_COUNT="$(grep -c '^data:' "$BODY" || true)"
if [[ "$DATA_COUNT" -lt 2 ]]; then
  echo "ASSERT FAIL: expected >=2 data: lines, got $DATA_COUNT" >&2
  exit 1
fi

# Spot-check JSON payload in events
assert_body_contains "$(cat "$BODY")" '"msg":"tick"' "sse payload"
assert_body_contains "$(cat "$BODY")" "id:" "sse id line"

echo "== SSE /once: two data events =="
BODY2="$(mktemp)"
curl -sS -N -D "$HDR" -o "$BODY2" --max-time 5 "http://127.0.0.1:${PORT}/once"
DATA2="$(grep -c '^data:' "$BODY2" || true)"
if [[ "$DATA2" -lt 2 ]]; then
  echo "ASSERT FAIL /once: expected >=2 data: lines, got $DATA2" >&2
  cat "$BODY2" >&2
  exit 1
fi
assert_body_contains "$(cat "$BODY2")" '"msg":"a"'
assert_body_contains "$(cat "$BODY2")" '"msg":"b"'

rm -f "$INIT" "$HDR" "$BODY" "$BODY2"
echo "PASS: stream.test.sh (SSE curl >=2 data: events, text/event-stream)"
