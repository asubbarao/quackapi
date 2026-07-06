#!/usr/bin/env bash
# compose_receipts.sh — prove every composability receipt end-to-end at Tier 1.
# Two-phase, mirroring test/conformance/driver_pure.py:
#   phase 1: handle_request() -> (status, content_type, body|handler_sql)
#   phase 2: dynamic routes -> execute the rendered handler_sql in a fresh session
# Output of this script IS the receipt block quoted in COMPOSABILITY.md.
set -uo pipefail   # no -e: a failed receipt must print, not kill the harness
cd "$(dirname "$0")/.."

S='~~QRCPT~~'                                   # sentinel to find our rows in .read noise
D=$'\x1e'                                       # record-separator delimiter

run_case() {                                     # $1 label, $2 method, $3 path, $4 body-sql-literal
  local label="$1" method="$2" path="$3" body="$4"
  local out line status ctype rbody hsql
  out=$(printf '.read framework.sql\n.read compose.sql\n.read compose_pg.sql\n.read compose_realtime.sql\nSELECT %s || status_code || %s || content_type || %s || coalesce(body, chr(1)) || %s || coalesce(handler_sql, chr(1)) FROM handle_request(%s, %s, %s, %s);\n' \
        "'$S'" "'$D'" "'$D'" "'$D'" "'$method'" "'$path'" "'{}'" "$body" \
        | duckdb -unsigned -list -noheader 2>&1 | grep -i -e "$S" -e "rror:" | head -1)
  line="${out#*"$S"}"
  status="${line%%"$D"*}"; line="${line#*"$D"}"
  ctype="${line%%"$D"*}";  line="${line#*"$D"}"
  rbody="${line%%"$D"*}"
  hsql="${line#*"$D"}"
  echo "== ${label}"
  echo "   ${method} ${path}   -> ${status} ${ctype}"
  if [ "$rbody" != $'\x01' ]; then
    echo "   body: ${rbody}"
  elif [ "$hsql" != $'\x01' ] && [ -n "$hsql" ]; then
    # phase 2: execute the rendered handler in a fresh session (pure-tier execution step)
    local exec_out
    exec_out=$(printf '.read framework.sql\n.read compose.sql\n.read compose_pg.sql\n.read compose_realtime.sql\nSELECT %s || body FROM ( %s ) t;\n' "'$S'" "$hsql" \
               | duckdb -unsigned -list -noheader 2>&1 | grep -i -e "$S" -e "rror:" | head -1)
    echo "   body: ${exec_out#*"$S"}"
  fi
  echo
}

run_case "R1 json_schema — valid doc"        POST '/check/order'  $'\'{"doc": "{\\"sku\\":\\"ABC123\\",\\"qty\\":5}"}\''
run_case "R1 json_schema — INVALID doc (qty>max, extra prop)" POST '/check/order' $'\'{"doc": "{\\"sku\\":\\"ABC123\\",\\"qty\\":5000,\\"hack\\":1}"}\''
run_case "R1 json_schema — unparseable doc"  POST '/check/order'  $'\'{"doc": "not json at all"}\''
run_case "R2 finetype — classify an IP"      GET  '/classify?value=192.168.1.1' 'NULL'
run_case "R2 finetype — classify an email"   GET  '/classify?value=alok@example.com' 'NULL'
run_case "R3 crypto — HMAC-sign payload"     POST '/webhooks/sign' $'\'{"payload": "order:12345:shipped"}\''
run_case "R4 tera — HTML report over live table" GET '/report' 'NULL'
run_case "R5 parser_tools — lint good SQL"   POST '/sql/lint'     $'\'{"q": "SELECT 1 FROM t WHERE x > 2"}\''
run_case "R5 parser_tools — lint bad SQL"    POST '/sql/lint'     $'\'{"q": "SELEKT oops FROM"}\''
run_case "R6 curl_httpfs — parallel fan-out (live network)" GET '/fanout' 'NULL'
run_case "422 guard — missing required body field" POST '/webhooks/sign' "'{}'"

run_case "R7 fts — BM25 search: pond"        GET  '/articles/search?q=pond' 'NULL'
run_case "R7 fts — BM25 search: database"    GET  '/articles/search?q=database' 'NULL'
run_case "R8 cronjob — schedule heartbeat"   POST '/jobs/heartbeat' $'\'{"schedule": "*/10 * * * * *"}\''
run_case "R9 bitfilters — known key"         GET  '/allowlist/check?id=key-bravo' 'NULL'
run_case "R9 bitfilters — unknown key"       GET  '/allowlist/check?id=intruder' 'NULL'
run_case "R10 rapidfuzz — typo lookup"       GET  '/users/fuzzy?name=alicce' 'NULL'
run_case "R11 markdown — md to html"         POST '/render/md' $'\'{"md": "A **bold** claim with a [link](https://duckdb.org) and *italics*"}\''
run_case "R12 postgres — list live pg table" GET  '/pg/products' 'NULL'
run_case "R12 postgres — one row, validated" GET  '/pg/products/2' 'NULL'
run_case "R12 postgres — 422 on bad id"      GET  '/pg/products/abc' 'NULL'

run_case "R13 redis — cache PUT (writes to live Redis)" POST '/cache' $'\'{"key": "user-7-prefs", "value": "dark-mode"}\''
run_case "R13 redis — cache GET (fresh process, state survived)" GET '/cache/user-7-prefs' 'NULL'
run_case "R13 redis — queue push (the broker slot)"  POST '/queue/push' $'\'{"task": "send-welcome-email:42"}\''
