#!/usr/bin/env bash
# HTTP integration: multipart/form-data single-file upload.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18944}"
INIT="$(mktemp /tmp/quackapi_multipart_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE upload POST '/upload' AS
SELECT $file::VARCHAR AS content, $file_filename::VARCHAR AS filename;

CREATE ROUTE upload_fields POST '/upload-fields' AS
SELECT $title::VARCHAR AS title, $file::VARCHAR AS content, $filename::VARCHAR AS filename;

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

# Minimal multipart body matching conformance case shape
MP_BODY=$'--testbnd\r\nContent-Disposition: form-data; name="file"; filename="test.txt"\r\n\r\nhello\r\n--testbnd--\r\n'

echo "-- 1. multipart_upload single file"
curl_json POST "/upload" \
  -H "Content-Type: multipart/form-data; boundary=testbnd" \
  --data-binary "$MP_BODY"
assert_status "$_QA_LAST_STATUS" "200" "multipart_upload"
assert_body_contains "$_QA_LAST_BODY" '"content":"hello"' "file content"
assert_body_contains "$_QA_LAST_BODY" '"filename":"test.txt"' "file filename"

echo "-- 2. multipart field + file"
MP2=$'--b2\r\nContent-Disposition: form-data; name="title"\r\n\r\nreadme\r\n--b2\r\nContent-Disposition: form-data; name="file"; filename="a.bin"\r\n\r\nXYZ\r\n--b2--\r\n'
curl_json POST "/upload-fields" \
  -H "Content-Type: multipart/form-data; boundary=b2" \
  --data-binary "$MP2"
assert_status "$_QA_LAST_STATUS" "200" "multipart_fields"
assert_body_contains "$_QA_LAST_BODY" '"title":"readme"' "title field"
assert_body_contains "$_QA_LAST_BODY" '"content":"XYZ"' "file bytes"
assert_body_contains "$_QA_LAST_BODY" '"filename":"a.bin"' "filename convenience"

echo "multipart.test.sh OK"
stop_quackapi
