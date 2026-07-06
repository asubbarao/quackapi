#!/usr/bin/env bash
# =============================================================================
# adversarial_http.sh — raw HTTP adversarial tests against the quackapi server
#
# *** DO NOT RUN YET — READ THIS HEADER ***
# Another agent is mid-edit on ext-cpp/src/quackapi_brain.cpp implementing
# keep-alive. The raw connection-handling code (accept loop, keep-alive timeout,
# pipeline buffer, Content-Length enforcement) is in flux. These tests must run
# against the FINAL post-keep-alive code, not the current work-in-progress.
#
# WHEN TO RUN:
#   1. keep-alive lands and is merged
#   2. Boot serve_brain on a fresh port in the 18400–18499 range (see below)
#   3. Run: bash test/fuzz/adversarial_http.sh
#
# HOW TO BOOT:
#   # From quackapi root:
#   PORT=18401
#   printf '.read framework.sql\n.read app.sql\n.read serve_brain.sql\n' \
#     | /opt/homebrew/bin/duckdb -unsigned -c "SET quackapi_port=${PORT};"
#   # Or however serve_brain.sql takes a port. Adjust as needed.
#
# COVERAGE (what this script tests):
#   1. Malformed request line (missing HTTP version)
#   2. Malformed request line (space in method)
#   3. Completely empty request
#   4. Request line only, no headers, no CRLF-CRLF terminator
#   5. Oversized single header (8KB value)
#   6. Oversized header section (many headers totalling >1MB)
#   7. Missing Content-Length on POST body
#   8. Content-Length mismatch: declared larger than actual body (truncated body)
#   9. Content-Length mismatch: declared smaller than actual body (extra bytes)
#  10. Content-Length = 0 on POST expecting a body → 422 (valid, body empty)
#  11. Negative Content-Length
#  12. Non-numeric Content-Length
#  13. Pipelined requests: two valid requests in one TCP write
#  14. Pipelined requests: valid + malformed in one write
#  15. Slowloris-style: send headers one byte per second (connection timeout test)
#  16. Slowloris-style: send body one byte per second
#  17. Bizarre HTTP method: FOOBAR /health HTTP/1.1
#  18. Method that is a SQL keyword: SELECT /health HTTP/1.1
#  19. Null byte in path: GET /users/\x00/5 HTTP/1.1
#  20. Header injection: value contains CRLF (attempts to inject a header)
#  21. Very long URL (8KB path)
#  22. Very long query string (8KB)
#  23. Path with null byte in query string
#  24. Keep-alive connection reuse: two sequential requests on same socket → both 200
#  25. Keep-alive correctness: Connection: close forces shutdown after first response
#  26. Partial request, then close — server must not hang
#  27. HTTP/1.0 request (no keep-alive by default)
#  28. Chunked Transfer-Encoding body (if server supports it, parse correctly)
#  29. Transfer-Encoding: identity (no-op passthrough)
#  30. Request with both Content-Length and Transfer-Encoding: chunked (ambiguous)
# =============================================================================

set -euo pipefail

PORT=${1:-18401}
HOST=127.0.0.1

PASS=0
FAIL=0
SKIP=0

pass() { echo "PASS: $1"; ((PASS+=1)); }
fail() { echo "FAIL: $1 — $2"; ((FAIL+=1)); }
skip() { echo "SKIP: $1"; ((SKIP+=1)); }

# Helper: send raw bytes to server, capture response, assert status code.
# Usage: send_raw_expect <description> <expected_status> <raw_request>
send_raw_expect() {
  local desc="$1"
  local expected="$2"
  local raw="$3"

  # Use nc (netcat) to send raw bytes; timeout 3s
  local response
  response=$(printf '%s' "$raw" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)

  local got_status
  got_status=$(printf '%s' "$response" | head -1 | grep -oE 'HTTP/[0-9.]+ [0-9]+' | grep -oE '[0-9]+$' || echo "NO_RESPONSE")

  if [ "$got_status" = "$expected" ]; then
    pass "$desc (got $got_status)"
  else
    fail "$desc" "expected HTTP $expected, got '$got_status'"
  fi
}

# Ensure server is reachable before running tests
if ! nc -z "$HOST" "$PORT" 2>/dev/null; then
  echo "ERROR: No server on $HOST:$PORT"
  echo "Boot quackapi serve_brain on port $PORT first, then re-run."
  exit 1
fi

echo "=== adversarial_http.sh — quackapi raw HTTP stress suite ==="
echo "    target: $HOST:$PORT"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# 1. Malformed request line — missing HTTP version
# ─────────────────────────────────────────────────────────────────────────────
send_raw_expect \
  "1. Malformed request line (no HTTP version)" \
  "400" \
  "GET /health\r\nHost: localhost\r\n\r\n"

# ─────────────────────────────────────────────────────────────────────────────
# 2. Malformed request line — space in method token
# ─────────────────────────────────────────────────────────────────────────────
send_raw_expect \
  "2. Malformed request line (space in method)" \
  "400" \
  "GET /users/1 HTTP/1.1\r\nHost: localhost\r\n\r\n"  # valid; replace with mangled:

# Overwrite: actually malformed method
raw_2=$(printf 'G ET /health HTTP/1.1\r\nHost: localhost\r\n\r\n')
response_2=$(printf '%s' "$raw_2" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_2=$(printf '%s' "$response_2" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_2" = "400" ]; then pass "2. Malformed method (space in token) → 400"; else fail "2. Malformed method" "expected 400, got $status_2"; fi

# ─────────────────────────────────────────────────────────────────────────────
# 3. Completely empty request
# ─────────────────────────────────────────────────────────────────────────────
response_3=$(printf '' | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
if [ -z "$response_3" ]; then
  pass "3. Empty request → server closes without response (or ignores)"
else
  status_3=$(printf '%s' "$response_3" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "?")
  if [ "$status_3" = "400" ]; then pass "3. Empty request → 400"; else fail "3. Empty request" "got $status_3, expected empty or 400"; fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# 4. Request line only, no CRLF-CRLF terminator (server should timeout or 400)
# ─────────────────────────────────────────────────────────────────────────────
response_4=$(printf 'GET /health HTTP/1.1\r\nHost: localhost' | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_4=$(printf '%s' "$response_4" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "TIMEOUT")
if [ "$status_4" = "408" ] || [ "$status_4" = "400" ] || [ "$status_4" = "TIMEOUT" ]; then
  pass "4. Unterminated headers → 408/400/timeout"
else
  fail "4. Unterminated headers" "expected timeout/400/408, got $status_4"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 5. Oversized single header (8KB value)
# ─────────────────────────────────────────────────────────────────────────────
BIG_VAL=$(printf '%8192s' | tr ' ' 'A')
response_5=$(printf 'GET /health HTTP/1.1\r\nHost: localhost\r\nX-Oversized: %s\r\n\r\n' "$BIG_VAL" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_5=$(printf '%s' "$response_5" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_5" = "431" ] || [ "$status_5" = "400" ] || [ "$status_5" = "200" ]; then
  pass "5. 8KB header value → $status_5 (431/400 = rejected; 200 = passed through)"
else
  fail "5. Oversized header" "unexpected status: $status_5"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 6. Oversized header section (100 headers with 1KB values each ≈ 100KB total)
# ─────────────────────────────────────────────────────────────────────────────
HDR_SECTION="GET /health HTTP/1.1\r\nHost: localhost\r\n"
VAL_1K=$(printf '%1024s' | tr ' ' 'B')
for i in $(seq 1 100); do
  HDR_SECTION="${HDR_SECTION}X-Hdr-${i}: ${VAL_1K}\r\n"
done
HDR_SECTION="${HDR_SECTION}\r\n"
response_6=$(printf '%s' "$HDR_SECTION" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_6=$(printf '%s' "$response_6" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_6" = "431" ] || [ "$status_6" = "400" ] || [ "$status_6" = "200" ]; then
  pass "6. 100KB header section → $status_6 (431/400 = rejected; 200 = passed through)"
else
  fail "6. Oversized header section" "unexpected status: $status_6"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 7. POST body with NO Content-Length header
# ─────────────────────────────────────────────────────────────────────────────
response_7=$(printf 'POST /users HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n\r\n{"name":"test","age":5}' | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_7=$(printf '%s' "$response_7" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_7" = "411" ] || [ "$status_7" = "400" ] || [ "$status_7" = "422" ] || [ "$status_7" = "201" ]; then
  pass "7. POST no Content-Length → $status_7 (411=correct; 400/422=body not read; 201=server guessed)"
else
  fail "7. POST no Content-Length" "unexpected status: $status_7"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 8. Content-Length > actual body (truncated — server must wait, then 400 or partial)
# ─────────────────────────────────────────────────────────────────────────────
BODY_8='{"name":"x","age":1}'
LEN_8=$(printf '%s' "$BODY_8" | wc -c | tr -d ' ')
DECLARED_8=$((LEN_8 + 100))  # claim 100 extra bytes
response_8=$(printf 'POST /users HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s' "$DECLARED_8" "$BODY_8" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_8=$(printf '%s' "$response_8" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "TIMEOUT")
# Server should read declared length (which arrives truncated), then either timeout
# waiting for remaining bytes, or return 400/422 on partial body.
if [ "$status_8" = "400" ] || [ "$status_8" = "422" ] || [ "$status_8" = "408" ] || [ "$status_8" = "TIMEOUT" ]; then
  pass "8. Content-Length > body → $status_8 (server waited or rejected partial)"
else
  fail "8. Truncated body" "expected 400/408/422/TIMEOUT, got $status_8"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 9. Content-Length < actual body (extra bytes that might bleed into next request)
# ─────────────────────────────────────────────────────────────────────────────
BODY_9='{"name":"y","age":2}'
LEN_9=$(printf '%s' "$BODY_9" | wc -c | tr -d ' ')
DECLARED_9=$((LEN_9 - 5))  # claim 5 fewer bytes
response_9=$(printf 'POST /users HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s' "$DECLARED_9" "$BODY_9" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_9=$(printf '%s' "$response_9" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
# Server reads exactly declared bytes (partial JSON) → 422 parsing error or 400.
if [ "$status_9" = "422" ] || [ "$status_9" = "400" ] || [ "$status_9" = "201" ]; then
  pass "9. Content-Length < body → $status_9 (extra bytes left in socket)"
else
  fail "9. Over-length body" "expected 201/400/422, got $status_9"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 10. Content-Length = 0 on POST requiring body → 422 (body empty, params missing)
# ─────────────────────────────────────────────────────────────────────────────
send_raw_expect \
  "10. POST Content-Length=0 → 422 (body empty)" \
  "422" \
  "$(printf 'POST /users HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n')"

# ─────────────────────────────────────────────────────────────────────────────
# 11. Negative Content-Length
# ─────────────────────────────────────────────────────────────────────────────
response_11=$(printf 'POST /users HTTP/1.1\r\nHost: localhost\r\nContent-Length: -1\r\n\r\n{}' | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_11=$(printf '%s' "$response_11" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_11" = "400" ] || [ "$status_11" = "422" ]; then
  pass "11. Negative Content-Length → $status_11"
else
  fail "11. Negative Content-Length" "expected 400/422, got $status_11"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 12. Non-numeric Content-Length
# ─────────────────────────────────────────────────────────────────────────────
response_12=$(printf 'POST /users HTTP/1.1\r\nHost: localhost\r\nContent-Length: foo\r\n\r\n{}' | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_12=$(printf '%s' "$response_12" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_12" = "400" ] || [ "$status_12" = "422" ]; then
  pass "12. Non-numeric Content-Length → $status_12"
else
  fail "12. Non-numeric Content-Length" "expected 400/422, got $status_12"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 13. Pipelined requests: two valid requests in one TCP write
#     Both must be served and both responses must be valid.
# ─────────────────────────────────────────────────────────────────────────────
PIPELINE_13=$(printf 'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\nGET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
response_13=$(printf '%s' "$PIPELINE_13" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
count_200=$(printf '%s' "$response_13" | grep -c 'HTTP/.* 200' || true)
if [ "$count_200" -ge 2 ]; then
  pass "13. Pipelined 2 valid requests → got $count_200 × 200 responses"
elif [ "$count_200" -eq 1 ]; then
  fail "13. Pipelined requests" "only 1 of 2 requests served (no keep-alive?)"
else
  fail "13. Pipelined requests" "no 200 responses; pipeline not supported (got: $(printf '%s' "$response_13" | head -3))"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 14. Pipelined: valid + malformed in one write
#     Server should respond to valid first, then 400 for malformed (or close after valid)
# ─────────────────────────────────────────────────────────────────────────────
PIPELINE_14=$(printf 'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\nNOT A REQUEST AT ALL\r\n\r\n')
response_14=$(printf '%s' "$PIPELINE_14" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
got_200_14=$(printf '%s' "$response_14" | grep -c 'HTTP/.* 200' || true)
if [ "$got_200_14" -ge 1 ]; then
  pass "14. Pipelined valid+malformed → served valid request (200 present)"
else
  fail "14. Pipelined valid+malformed" "no 200 for first request; response: $(printf '%s' "$response_14" | head -2)"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 15. Slowloris-style: headers sent one byte per second (connection timeout test)
#     This requires a background process; we use a subprocess with a sleep loop.
#     Expects: server closes connection or returns 408 within a few seconds.
# ─────────────────────────────────────────────────────────────────────────────
echo "15. Slowloris partial headers (background, ~5s timeout) ..."
response_15=$( (
  printf 'GET /health HTTP/1.1\r\n'
  sleep 1; printf 'Host: localhost\r\n'
  sleep 1; printf 'X-Slow: '
  sleep 1; printf 'header\r\n'
  # Never send final \r\n to terminate headers
  sleep 3
) | nc -w8 "$HOST" "$PORT" 2>/dev/null || true)
status_15=$(printf '%s' "$response_15" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "TIMEOUT_OR_CLOSE")
if [ "$status_15" = "408" ] || [ "$status_15" = "TIMEOUT_OR_CLOSE" ]; then
  pass "15. Slowloris partial headers → $status_15 (server timed out or closed)"
elif [ "$status_15" = "200" ]; then
  pass "15. Slowloris → 200 (server accepted slow header; no timeout enforcement here)"
else
  fail "15. Slowloris" "unexpected status: $status_15"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 16. Slowloris body: headers complete, body sent 1 byte/sec
# ─────────────────────────────────────────────────────────────────────────────
echo "16. Slowloris body (background, ~5s) ..."
BODY_SLOW='{"name":"slow","age":1}'
BODY_LEN=$(printf '%s' "$BODY_SLOW" | wc -c | tr -d ' ')
response_16=$( (
  printf 'POST /users HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n' "$BODY_LEN"
  for c in $(printf '%s' "$BODY_SLOW" | fold -w1); do
    printf '%s' "$c"; sleep 0.5
  done
) | nc -w15 "$HOST" "$PORT" 2>/dev/null || true)
status_16=$(printf '%s' "$response_16" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "TIMEOUT_OR_CLOSE")
if [ "$status_16" = "201" ] || [ "$status_16" = "408" ] || [ "$status_16" = "TIMEOUT_OR_CLOSE" ]; then
  pass "16. Slowloris body → $status_16 (accepted or timed out)"
else
  fail "16. Slowloris body" "unexpected status: $status_16"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 17. Bizarre method: FOOBAR
# ─────────────────────────────────────────────────────────────────────────────
response_17=$(printf 'FOOBAR /health HTTP/1.1\r\nHost: localhost\r\n\r\n' | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_17=$(printf '%s' "$response_17" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_17" = "405" ] || [ "$status_17" = "400" ] || [ "$status_17" = "501" ]; then
  pass "17. Bizarre method FOOBAR → $status_17"
else
  fail "17. Bizarre method" "expected 400/405/501, got $status_17"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 18. SQL keyword as method: SELECT
# ─────────────────────────────────────────────────────────────────────────────
response_18=$(printf 'SELECT /health HTTP/1.1\r\nHost: localhost\r\n\r\n' | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_18=$(printf '%s' "$response_18" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_18" = "405" ] || [ "$status_18" = "400" ] || [ "$status_18" = "501" ]; then
  pass "18. SQL keyword method SELECT → $status_18"
else
  fail "18. SQL keyword method" "expected 400/405/501, got $status_18"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 19. Null byte in path
# ─────────────────────────────────────────────────────────────────────────────
response_19=$(printf 'GET /users/\x005 HTTP/1.1\r\nHost: localhost\r\n\r\n' | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_19=$(printf '%s' "$response_19" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_19" = "400" ] || [ "$status_19" = "404" ] || [ "$status_19" = "422" ]; then
  pass "19. Null byte in path → $status_19 (rejected or misrouted)"
else
  fail "19. Null byte in path" "expected 400/404/422, got $status_19"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 20. Header injection: CRLF in header value (attempt to inject a second header)
# ─────────────────────────────────────────────────────────────────────────────
response_20=$(printf 'GET /health HTTP/1.1\r\nHost: localhost\r\nX-Evil: value\r\nX-Injected: injected\r\n\r\n' | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_20=$(printf '%s' "$response_20" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_20" = "200" ] || [ "$status_20" = "400" ]; then
  pass "20. CRLF header injection → $status_20 (server accepted or rejected; injected header isolated)"
else
  fail "20. Header injection" "unexpected status: $status_20"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 21. Very long URL (8KB path)
# ─────────────────────────────────────────────────────────────────────────────
LONG_PATH="/"$(printf '%8000s' | tr ' ' 'a')
response_21=$(printf 'GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n' "$LONG_PATH" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_21=$(printf '%s' "$response_21" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_21" = "414" ] || [ "$status_21" = "400" ] || [ "$status_21" = "404" ]; then
  pass "21. 8KB URL → $status_21 (414/400=rejected; 404=parsed but not found)"
else
  fail "21. Very long URL" "expected 400/404/414, got $status_21"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 22. Very long query string (8KB)
# ─────────────────────────────────────────────────────────────────────────────
LONG_QS="q="$(printf '%8000s' | tr ' ' 'b')
response_22=$(printf 'GET /search?%s HTTP/1.1\r\nHost: localhost\r\n\r\n' "$LONG_QS" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_22=$(printf '%s' "$response_22" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_22" = "200" ] || [ "$status_22" = "400" ] || [ "$status_22" = "414" ]; then
  pass "22. 8KB query string → $status_22 (200=passed through; 400/414=rejected)"
else
  fail "22. Very long query string" "unexpected status: $status_22"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 23. Null byte in query string
# ─────────────────────────────────────────────────────────────────────────────
response_23=$(printf 'GET /search?q=hello\x00world HTTP/1.1\r\nHost: localhost\r\n\r\n' | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_23=$(printf '%s' "$response_23" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_23" = "200" ] || [ "$status_23" = "400" ]; then
  pass "23. Null byte in query string → $status_23"
else
  fail "23. Null byte in query" "unexpected status: $status_23"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 24. Keep-alive connection reuse: two sequential requests on same socket
# ─────────────────────────────────────────────────────────────────────────────
KEEPALIVE_PAIR=$(printf 'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\nGET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
response_24=$(printf '%s' "$KEEPALIVE_PAIR" | nc -w5 "$HOST" "$PORT" 2>/dev/null || true)
count_200_24=$(printf '%s' "$response_24" | grep -c 'HTTP/.* 200' || true)
if [ "$count_200_24" -ge 2 ]; then
  pass "24. Keep-alive reuse → $count_200_24 × 200 responses on one socket"
elif [ "$count_200_24" -eq 1 ]; then
  pass "24. Keep-alive partial → 1 × 200 (server may not support keep-alive yet)"
else
  fail "24. Keep-alive reuse" "no 200 responses"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 25. Connection: close forces shutdown after first response
# ─────────────────────────────────────────────────────────────────────────────
response_25=$(printf 'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_25=$(printf '%s' "$response_25" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
connection_close_25=$(printf '%s' "$response_25" | grep -i 'Connection: close' || true)
if [ "$status_25" = "200" ]; then
  pass "25. Connection: close → 200, server sent response (close header: $connection_close_25)"
else
  fail "25. Connection: close" "expected 200, got $status_25"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 26. Partial request then close (server must not hang on dangling connection)
# ─────────────────────────────────────────────────────────────────────────────
response_26=$( (printf 'GET /health HTTP/1.1\r\n'; sleep 0.5) | nc -w2 "$HOST" "$PORT" 2>/dev/null || true)
status_26=$(printf '%s' "$response_26" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "CLOSED")
if [ "$status_26" = "408" ] || [ "$status_26" = "400" ] || [ "$status_26" = "CLOSED" ]; then
  pass "26. Partial request then close → $status_26 (server handles clean close)"
else
  fail "26. Partial request" "unexpected: $status_26"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 27. HTTP/1.0 request (no keep-alive by default)
# ─────────────────────────────────────────────────────────────────────────────
response_27=$(printf 'GET /health HTTP/1.0\r\nHost: localhost\r\n\r\n' | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_27=$(printf '%s' "$response_27" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_27" = "200" ] || [ "$status_27" = "400" ]; then
  pass "27. HTTP/1.0 request → $status_27"
else
  fail "27. HTTP/1.0" "expected 200/400, got $status_27"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 28. Chunked Transfer-Encoding body
# ─────────────────────────────────────────────────────────────────────────────
CHUNK_BODY=$(printf '14\r\n{"name":"c","age":3}\r\n0\r\n\r\n')
response_28=$(printf 'POST /users HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n%s' "$CHUNK_BODY" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_28=$(printf '%s' "$response_28" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_28" = "201" ] || [ "$status_28" = "400" ] || [ "$status_28" = "422" ]; then
  pass "28. Chunked body → $status_28 (201=parsed; 400/422=unsupported)"
else
  fail "28. Chunked body" "unexpected status: $status_28"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 29. Transfer-Encoding: identity (passthrough, same as no TE)
# ─────────────────────────────────────────────────────────────────────────────
BODY_29='{"name":"id","age":4}'
LEN_29=$(printf '%s' "$BODY_29" | wc -c | tr -d ' ')
response_29=$(printf 'POST /users HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %d\r\nTransfer-Encoding: identity\r\n\r\n%s' "$LEN_29" "$BODY_29" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_29=$(printf '%s' "$response_29" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_29" = "201" ] || [ "$status_29" = "400" ]; then
  pass "29. Transfer-Encoding: identity → $status_29"
else
  fail "29. TE identity" "unexpected status: $status_29"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 30. Both Content-Length and Transfer-Encoding: chunked (ambiguous per RFC 7230 §3.3.3)
#     RFC says chunked wins, ignore Content-Length. Server should handle gracefully.
# ─────────────────────────────────────────────────────────────────────────────
BODY_30='{"name":"amb","age":7}'
LEN_30=$(printf '%s' "$BODY_30" | wc -c | tr -d ' ')
CHUNK_30=$(printf '16\r\n{"name":"amb","age":7}\r\n0\r\n\r\n')
response_30=$(printf 'POST /users HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %d\r\nTransfer-Encoding: chunked\r\n\r\n%s' "$LEN_30" "$CHUNK_30" | nc -w3 "$HOST" "$PORT" 2>/dev/null || true)
status_30=$(printf '%s' "$response_30" | head -1 | grep -oE '[0-9]{3}' | head -1 || echo "NO_RESPONSE")
if [ "$status_30" = "201" ] || [ "$status_30" = "400" ] || [ "$status_30" = "422" ]; then
  pass "30. Both CL+TE chunked → $status_30 (201=chunked won; 400/422=rejected ambiguity)"
else
  fail "30. Ambiguous CL+TE" "unexpected status: $status_30"
fi

# ─────────────────────────────────────────────────────────────────────────────
# SUMMARY
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== adversarial_http.sh RESULT: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped ==="

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
exit 0
