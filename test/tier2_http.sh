#!/usr/bin/env bash
# =============================================================================
# Tier-2: HTTP assertions against the live quackapi server
# Usage:  bash test/tier2_http.sh [BASE_URL] [--post]
# Default BASE_URL: http://127.0.0.1:18099
#
# All GET checks are read-only and safe against the live server.
# POST /users only runs when --post is passed (it mutates the users table).
# =============================================================================

BASE_URL="${1:-http://127.0.0.1:18099}"
PASS=0
FAIL=0

_ok()  { PASS=$((PASS+1)); printf "  PASS  %s\n" "$1"; }
_fail(){ FAIL=$((FAIL+1)); printf "  FAIL  %s\n  ↳ %s\n" "$1" "$2"; }

check() {
  local name="$1"; local ok="$2"; local detail="$3"
  if [ "$ok" = "1" ]; then _ok "$name"; else _fail "$name" "$detail"; fi
}

# Run a DuckDB SQL expression returning one scalar; strip noise.
dq() { duckdb -noheader -list -c "$1" 2>/dev/null | tr -d ' \r\n'; }

# Escape single quotes for safe SQL literal embedding.
sql_esc() { printf '%s' "$1" | sed "s/'/\\''/g"; }

# Fetch body and status separately (two curl calls — simple and macOS-safe).
fetch_body()   { curl -s --connect-timeout 5 "$1" 2>/dev/null; }
fetch_status() { curl -s -o /dev/null -w '%{http_code}' --connect-timeout 5 "$1" 2>/dev/null; }
post_body()    { curl -s --connect-timeout 5 -X POST -H 'Content-Type: application/json' -d "$2" "$1" 2>/dev/null; }
post_status()  { curl -s -o /dev/null -w '%{http_code}' --connect-timeout 5 -X POST -H 'Content-Type: application/json' -d "$2" "$1" 2>/dev/null; }

echo ""
echo "========================================================"
echo "  quackapi Tier-2 HTTP tests  →  $BASE_URL"
echo "========================================================"

# ─────────────────────────────────────────────────────────────────────────────
# 0  Connectivity gate — abort with clear message if server is unreachable
# ─────────────────────────────────────────────────────────────────────────────
PROBE=$(fetch_status "$BASE_URL/users")
if [ "$PROBE" = "000" ]; then
  echo ""
  echo "  SKIP  Server not reachable at $BASE_URL"
  echo "        Start the server first:"
  echo "          duckdb < launch_server.sql &   # run from the repo root"
  echo "        Then re-run:  bash test/tier2_http.sh"
  echo ""
  exit 2
fi

# ─────────────────────────────────────────────────────────────────────────────
# 1  GET /users/1  →  200, JSON with id=1, name=alice
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "── GET /users/1 ──────────────────────────────────────"
STATUS=$(fetch_status "$BASE_URL/users/1")
BODY=$(fetch_body "$BASE_URL/users/1")
check "GET /users/1 → status 200"     "$([ "$STATUS" = "200" ] && echo 1 || echo 0)"  "status=$STATUS"
check "GET /users/1 → body not empty" "$([ -n "$BODY" ] && echo 1 || echo 0)"          "body is empty"
ID_VAL=$(dq "SELECT json_extract_string('$(sql_esc "$BODY")', '$.id') AS v;")
check "GET /users/1 → $.id = 1"       "$([ "$ID_VAL" = "1" ] && echo 1 || echo 0)"   "id=$ID_VAL body=${BODY:0:80}"

# ─────────────────────────────────────────────────────────────────────────────
# 2  GET /users/abc  →  422 + detail array with int_parsing
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "── GET /users/abc ────────────────────────────────────"
STATUS=$(fetch_status "$BASE_URL/users/abc")
BODY=$(fetch_body "$BASE_URL/users/abc")
check "GET /users/abc → status 422"  "$([ "$STATUS" = "422" ] && echo 1 || echo 0)"  "status=$STATUS"
INT_HIT=$(dq "SELECT array_length(list_filter(string_split('$(sql_esc "$BODY")', '\"'), lambda t: t = 'int_parsing'))::VARCHAR AS v;")
check "GET /users/abc → detail has int_parsing" "$([ "${INT_HIT:-0}" -gt 0 ] && echo 1 || echo 0)" "body=${BODY:0:120}"

# ─────────────────────────────────────────────────────────────────────────────
# 3  GET /users  →  200, JSON array
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "── GET /users ────────────────────────────────────────"
STATUS=$(fetch_status "$BASE_URL/users")
BODY=$(fetch_body "$BASE_URL/users")
check "GET /users → status 200"         "$([ "$STATUS" = "200" ] && echo 1 || echo 0)"  "status=$STATUS"
FIRST_CHAR=$(printf '%s' "$BODY" | cut -c1)
check "GET /users → body is JSON array" "$([ "$FIRST_CHAR" = "[" ] && echo 1 || echo 0)" "first_char=$FIRST_CHAR body=${BODY:0:60}"

# ─────────────────────────────────────────────────────────────────────────────
# 4  GET /openapi.json  →  200, valid OpenAPI JSON
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "── GET /openapi.json ─────────────────────────────────"
STATUS=$(fetch_status "$BASE_URL/openapi.json")
BODY=$(fetch_body "$BASE_URL/openapi.json")
check "GET /openapi.json → status 200"  "$([ "$STATUS" = "200" ] && echo 1 || echo 0)"  "status=$STATUS"

OAI_VER=$(dq "SELECT json_extract_string('$(sql_esc "$BODY")', '$.openapi') AS v;")
check "GET /openapi.json → $.openapi = 3.0.0"  "$([ "$OAI_VER" = "3.0.0" ] && echo 1 || echo 0)"  "openapi=$OAI_VER"

OAI_TITLE=$(dq "SELECT json_extract_string('$(sql_esc "$BODY")', '$.info.title') AS v;")
check "GET /openapi.json → $.info.title = quackapi" "$([ "$OAI_TITLE" = "quackapi" ] && echo 1 || echo 0)" "title=$OAI_TITLE"

# paths must be a JSON object (starts with {)
PATHS_FIRST=$(dq "SELECT substr(json_extract('$(sql_esc "$BODY")'::JSON, '$.paths')::VARCHAR, 1, 1) AS v;")
check "GET /openapi.json → $.paths is object"  "$([ "$PATHS_FIRST" = "{" ] && echo 1 || echo 0)"  "paths_first=$PATHS_FIRST"

# ─────────────────────────────────────────────────────────────────────────────
# 5  GET /docs  →  200, text/html, swagger-ui div present
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "── GET /docs ─────────────────────────────────────────"
STATUS=$(fetch_status "$BASE_URL/docs")
BODY=$(fetch_body "$BASE_URL/docs")
check "GET /docs → status 200"  "$([ "$STATUS" = "200" ] && echo 1 || echo 0)"  "status=$STATUS"

STARTS_DOCTYPE=$(printf '%s' "$BODY" | cut -c1-15)
check "GET /docs → starts with DOCTYPE"  "$([ "$STARTS_DOCTYPE" = "<!DOCTYPE html>" ] && echo 1 || echo 0)" "got: $STARTS_DOCTYPE"

SWAG_HIT=$(dq "SELECT array_length(list_filter(string_split('$(sql_esc "$BODY")', 'id='), lambda t: starts_with(t,'\"swagger-ui\"')))::VARCHAR AS v;")
check "GET /docs → swagger-ui div present"  "$([ "${SWAG_HIT:-0}" -gt 0 ] && echo 1 || echo 0)"  "swag_hit=$SWAG_HIT"

# ─────────────────────────────────────────────────────────────────────────────
# 6  GET /nope  →  404
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "── GET /nope ─────────────────────────────────────────"
STATUS=$(fetch_status "$BASE_URL/nope")
check "GET /nope → status 404"  "$([ "$STATUS" = "404" ] && echo 1 || echo 0)"  "status=$STATUS"

# ─────────────────────────────────────────────────────────────────────────────
# 7  GET /search?q=hi&limit=999  →  422 less_than_equal
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "── GET /search?q=hi&limit=999 ────────────────────────"
STATUS=$(fetch_status "${BASE_URL}/search?q=hi&limit=999")
BODY=$(fetch_body "${BASE_URL}/search?q=hi&limit=999")
check "GET /search?limit=999 → status 422"  "$([ "$STATUS" = "422" ] && echo 1 || echo 0)"  "status=$STATUS"
LTE_HIT=$(dq "SELECT array_length(list_filter(string_split('$(sql_esc "$BODY")', '\"'), lambda t: t = 'less_than_equal'))::VARCHAR AS v;")
check "GET /search?limit=999 → detail has less_than_equal"  "$([ "${LTE_HIT:-0}" -gt 0 ] && echo 1 || echo 0)"  "body=${BODY:0:120}"

# ─────────────────────────────────────────────────────────────────────────────
# 8  POST /users  —  only when --post flag is present (mutates state)
# ─────────────────────────────────────────────────────────────────────────────
echo ""
POST_ENABLED=0
for arg in "$@"; do [ "$arg" = "--post" ] && POST_ENABLED=1; done

if [ "$POST_ENABLED" = "1" ]; then
  echo "── POST /users (--post enabled) ─────────────────────"
  STATUS=$(post_status "$BASE_URL/users" '{"name":"tiertest","age":77}')
  BODY=$(post_body   "$BASE_URL/users" '{"name":"tiertest","age":77}')
  check "POST /users valid → status 200"   "$([ "$STATUS" = "200" ] && echo 1 || echo 0)"  "status=$STATUS body=${BODY:0:80}"
  NAME_VAL=$(dq "SELECT json_extract_string('$(sql_esc "$BODY")', '$.name') AS v;")
  check "POST /users valid → $.name = tiertest"  "$([ "$NAME_VAL" = "tiertest" ] && echo 1 || echo 0)"  "name=$NAME_VAL"

  STATUS=$(post_status "$BASE_URL/users" '{"name":"noage"}')
  check "POST /users missing age → status 422"  "$([ "$STATUS" = "422" ] && echo 1 || echo 0)"  "status=$STATUS"
else
  echo "── POST /users  (skipped — pass --post to enable) ───"
  echo "  NOTE  POST mutates the live users table; opt-in with: bash test/tier2_http.sh [BASE_URL] --post"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "========================================================"
TOTAL=$((PASS + FAIL))
printf "  TOTAL %d   PASS %d   FAIL %d\n" "$TOTAL" "$PASS" "$FAIL"
echo "========================================================"
echo ""

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
