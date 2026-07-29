#!/usr/bin/env bash
# HTTP integration: FORMAT ndjson/csv/parquet/arrow + Accept negotiation.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18971}"
INIT="$(mktemp /tmp/quackapi_format_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE items GET '/items' AS
  SELECT * FROM (VALUES (1, 'alice'), (2, 'bob,jr')) AS t(id, name);

CREATE ROUTE items_ndjson GET '/items_ndjson' FORMAT ndjson AS
  SELECT * FROM (VALUES (1, 'alice'), (2, 'bob')) AS t(id, name);

CREATE ROUTE items_csv GET '/items_csv' FORMAT csv AS
  SELECT * FROM (VALUES (1, 'alice'), (2, 'bob,jr')) AS t(id, name);

CREATE ROUTE items_json_fmt GET '/items_json_fmt' FORMAT json AS
  SELECT 1 AS id, 'x' AS name;

CREATE ROUTE items_parquet GET '/items_parquet' FORMAT parquet AS
  SELECT * FROM (VALUES (1, 'alice'), (2, 'bob,jr')) AS t(id, name);

CREATE ROUTE items_arrow GET '/items_arrow' FORMAT arrow AS
  SELECT * FROM (VALUES (1, 'alice'), (2, 'bob,jr')) AS t(id, name);
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

header_ct() {
  echo "$_QA_LAST_HEADERS" | tr -d '\r' | grep -i '^Content-Type:' | head -1
}

# Binary body fetch (bash $(cat) strips NULs — keep bytes on disk).
# Sets: _QA_LAST_STATUS, _QA_LAST_HEADERS, _QA_LAST_BODY_FILE
curl_binary() {
  local method="$1" path="$2"
  shift 2
  local url="http://127.0.0.1:${_QA_PORT}${path}"
  local tmp hdr
  tmp="$(mktemp)"
  hdr="$(mktemp)"
  set +e
  local err
  err="$(curl -sS -D "$hdr" -o "$tmp" -X "$method" "$@" "$url" 2>&1)"
  local rc=$?
  set -e
  if [[ $rc -ne 0 ]]; then
    _QA_LAST_STATUS="0"
    _QA_LAST_HEADERS=""
    _QA_LAST_BODY_FILE=""
    rm -f "$tmp" "$hdr"
    echo "curl failed rc=$rc: ${err}" >&2
    return 0
  fi
  _QA_LAST_BODY_FILE="$tmp"
  _QA_LAST_HEADERS="$(cat "$hdr")"
  _QA_LAST_STATUS="$(awk 'NR==1 {print $2}' "$hdr")"
  rm -f "$hdr"
}

assert_parquet_magic() {
  local file="$1" label="${2:-parquet_magic}"
  local magic
  magic="$(head -c 4 "$file" 2>/dev/null || true)"
  if [[ "$magic" != "PAR1" ]]; then
    echo "ASSERT FAIL ($label): expected PAR1 magic, got: $(xxd -l 4 -p "$file" 2>/dev/null || echo missing)" >&2
    return 1
  fi
}

echo "-- 1. default route still JSON array"
curl_json GET "/items"
assert_status "$_QA_LAST_STATUS" "200" "items_json"
assert_body_contains "$_QA_LAST_BODY" '"id":1' "items_json id"
assert_body_contains "$_QA_LAST_BODY" '[' "items_json array"
if ! header_ct | grep -qi 'application/json'; then
  echo "ASSERT FAIL: Content-Type not application/json: $(header_ct)" >&2
  exit 1
fi

echo "-- 2. Accept: text/csv on default FORMAT → CSV"
curl_json GET "/items" -H "Accept: text/csv"
assert_status "$_QA_LAST_STATUS" "200" "items_accept_csv"
assert_body_contains "$_QA_LAST_BODY" 'id,name' "csv header"
assert_body_contains "$_QA_LAST_BODY" '1,alice' "csv row1"
assert_body_contains "$_QA_LAST_BODY" '"bob,jr"' "csv escaped comma"
if ! header_ct | grep -qi 'text/csv'; then
  echo "ASSERT FAIL: Content-Type not text/csv: $(header_ct)" >&2
  exit 1
fi

echo "-- 3. Accept: application/x-ndjson on default FORMAT → NDJSON"
curl_json GET "/items" -H "Accept: application/x-ndjson"
assert_status "$_QA_LAST_STATUS" "200" "items_accept_ndjson"
assert_body_contains "$_QA_LAST_BODY" '{"id":1,"name":"alice"}' "ndjson row1"
assert_body_not_contains "$_QA_LAST_BODY" '[' "ndjson not array"
if ! header_ct | grep -qi 'application/x-ndjson'; then
  echo "ASSERT FAIL: Content-Type not application/x-ndjson: $(header_ct)" >&2
  exit 1
fi

echo "-- 4. Accept: application/jsonl also → NDJSON"
curl_json GET "/items" -H "Accept: application/jsonl"
assert_status "$_QA_LAST_STATUS" "200" "items_accept_jsonl"
assert_body_contains "$_QA_LAST_BODY" '{"id":2,"name":"bob,jr"}' "jsonl row2"
if ! header_ct | grep -qi 'application/x-ndjson'; then
  echo "ASSERT FAIL: Content-Type not application/x-ndjson for jsonl: $(header_ct)" >&2
  exit 1
fi

echo "-- 5. FORMAT ndjson route (explicit; Accept ignored for format)"
curl_json GET "/items_ndjson" -H "Accept: application/json"
assert_status "$_QA_LAST_STATUS" "200" "fmt_ndjson"
assert_body_contains "$_QA_LAST_BODY" '{"id":1,"name":"alice"}' "fmt_ndjson body"
if ! header_ct | grep -qi 'application/x-ndjson'; then
  echo "ASSERT FAIL: explicit FORMAT ndjson Content-Type: $(header_ct)" >&2
  exit 1
fi

echo "-- 6. FORMAT csv route (explicit wins over Accept: application/json)"
curl_json GET "/items_csv" -H "Accept: application/json"
assert_status "$_QA_LAST_STATUS" "200" "fmt_csv"
assert_body_contains "$_QA_LAST_BODY" 'id,name' "fmt_csv header"
assert_body_contains "$_QA_LAST_BODY" '"bob,jr"' "fmt_csv escaped"
if ! header_ct | grep -qi 'text/csv'; then
  echo "ASSERT FAIL: explicit FORMAT csv Content-Type: $(header_ct)" >&2
  exit 1
fi

echo "-- 7. FORMAT json explicit still JSON (Accept: text/csv still negotiates)"
curl_json GET "/items_json_fmt" -H "Accept: text/csv"
assert_status "$_QA_LAST_STATUS" "200" "fmt_json_accept_csv"
assert_body_contains "$_QA_LAST_BODY" 'id,name' "fmt_json negotiates csv"
if ! header_ct | grep -qi 'text/csv'; then
  echo "ASSERT FAIL: FORMAT json should honor Accept text/csv: $(header_ct)" >&2
  exit 1
fi

echo "-- 8. FORMAT parquet route (explicit; Accept ignored; PAR1 magic)"
curl_binary GET "/items_parquet" -H "Accept: application/json"
assert_status "$_QA_LAST_STATUS" "200" "fmt_parquet"
assert_parquet_magic "$_QA_LAST_BODY_FILE" "fmt_parquet"
if ! header_ct | grep -qi 'application/vnd.apache.parquet'; then
  echo "ASSERT FAIL: explicit FORMAT parquet Content-Type: $(header_ct)" >&2
  exit 1
fi
# Round-trip: DuckDB can read the HTTP body as parquet
"$DUCKDB_BIN" -unsigned -c "SELECT id, name FROM read_parquet('${_QA_LAST_BODY_FILE}') ORDER BY id" | grep -q alice
rm -f "$_QA_LAST_BODY_FILE"

echo "-- 9. Accept: application/vnd.apache.parquet on default FORMAT → Parquet"
curl_binary GET "/items" -H "Accept: application/vnd.apache.parquet"
assert_status "$_QA_LAST_STATUS" "200" "items_accept_parquet"
assert_parquet_magic "$_QA_LAST_BODY_FILE" "items_accept_parquet"
if ! header_ct | grep -qi 'application/vnd.apache.parquet'; then
  echo "ASSERT FAIL: Accept parquet Content-Type: $(header_ct)" >&2
  exit 1
fi
rm -f "$_QA_LAST_BODY_FILE"

echo "-- 10. Accept: application/parquet alias → Parquet"
curl_binary GET "/items" -H "Accept: application/parquet"
assert_status "$_QA_LAST_STATUS" "200" "items_accept_parquet_alias"
assert_parquet_magic "$_QA_LAST_BODY_FILE" "items_accept_parquet_alias"
if ! header_ct | grep -qi 'application/vnd.apache.parquet'; then
  echo "ASSERT FAIL: Accept application/parquet Content-Type: $(header_ct)" >&2
  exit 1
fi
rm -f "$_QA_LAST_BODY_FILE"

assert_arrow_stream_magic() {
  local file="$1" label="${2:-arrow_stream_magic}"
  # IPC stream continuation marker: first 4 bytes 0xFFFFFFFF
  local magic
  magic="$(xxd -l 4 -p "$file" 2>/dev/null || true)"
  if [[ "$magic" != "ffffffff" ]]; then
    echo "ASSERT FAIL ($label): expected ffffffff stream magic, got: ${magic:-missing}" >&2
    return 1
  fi
}

echo "-- 11. FORMAT arrow route (explicit; IPC stream magic; nanoarrow)"
curl_binary GET "/items_arrow" -H "Accept: application/json"
assert_status "$_QA_LAST_STATUS" "200" "fmt_arrow"
assert_arrow_stream_magic "$_QA_LAST_BODY_FILE" "fmt_arrow"
if ! header_ct | grep -qi 'application/vnd.apache.arrow.stream'; then
  echo "ASSERT FAIL: explicit FORMAT arrow Content-Type: $(header_ct)" >&2
  exit 1
fi
# Round-trip: DuckDB + nanoarrow can read the HTTP body
"$DUCKDB_BIN" -unsigned -c "
LOAD nanoarrow;
SELECT id, name FROM read_arrow('${_QA_LAST_BODY_FILE}') ORDER BY id;
" | grep -q alice
rm -f "$_QA_LAST_BODY_FILE"

echo "-- 12. Accept: application/vnd.apache.arrow.stream on default FORMAT → Arrow"
curl_binary GET "/items" -H "Accept: application/vnd.apache.arrow.stream"
assert_status "$_QA_LAST_STATUS" "200" "items_accept_arrow"
assert_arrow_stream_magic "$_QA_LAST_BODY_FILE" "items_accept_arrow"
if ! header_ct | grep -qi 'application/vnd.apache.arrow.stream'; then
  echo "ASSERT FAIL: Accept arrow stream Content-Type: $(header_ct)" >&2
  exit 1
fi
rm -f "$_QA_LAST_BODY_FILE"

echo "-- 13. Accept: application/vnd.apache.arrow.file alias → Arrow stream body"
curl_binary GET "/items" -H "Accept: application/vnd.apache.arrow.file"
assert_status "$_QA_LAST_STATUS" "200" "items_accept_arrow_file"
assert_arrow_stream_magic "$_QA_LAST_BODY_FILE" "items_accept_arrow_file"
if ! header_ct | grep -qi 'application/vnd.apache.arrow.stream'; then
  echo "ASSERT FAIL: Accept arrow.file Content-Type (we still emit stream): $(header_ct)" >&2
  exit 1
fi
rm -f "$_QA_LAST_BODY_FILE"

echo "format.test.sh OK"
stop_quackapi