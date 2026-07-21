#!/usr/bin/env bash
# HTTP: empty-body POST/PUT (no Content-Length / Transfer-Encoding) must not hang.
# Regression for the vendored httplib RFC 7230 §3.3.3 fix in third_party/httplib.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18995}"
INIT="$(mktemp /tmp/quackapi_empty_post_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE ROUTE ping POST '/ping' PARAM name VARCHAR AS SELECT 'hi ' || $name AS msg;
CREATE ROUTE put_ping PUT '/put_ping' PARAM name VARCHAR AS SELECT 'put ' || $name AS msg;
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

# Raw empty-body request helper: no -d, no Content-Length, hard timeout.
# Before the fix curl hangs until --max-time (exit 28); after: immediate 200.
empty_method() {
  local method="$1" path="$2" label="$3"
  local tmp status rc
  tmp="$(mktemp)"
  set +e
  status="$(curl -sS --max-time 5 -o "$tmp" -w '%{http_code}' -X "$method" \
    "http://127.0.0.1:${_QA_PORT}${path}" 2>/tmp/quackapi_empty_${label}_err.txt)"
  rc=$?
  set -e
  local body
  body="$(cat "$tmp")"
  rm -f "$tmp"
  echo "  rc=$rc status=$status body=$body"
  if [[ "$rc" -ne 0 ]]; then
    echo "ASSERT FAIL ($label): curl exit $rc (28=timeout hang)" >&2
    cat "/tmp/quackapi_empty_${label}_err.txt" >&2 || true
    return 1
  fi
  assert_status "$status" "200" "$label"
  _QA_LAST_BODY="$body"
  _QA_LAST_STATUS="$status"
}

echo "-- 1. raw empty-body POST /ping?name=world (no -d, --max-time 5)"
empty_method POST "/ping?name=world" "empty_post"
assert_body_contains "$_QA_LAST_BODY" 'hi world' "empty_post body"
assert_body_contains "$_QA_LAST_BODY" '"msg"' "empty_post msg field"

echo "-- 2. raw empty-body PUT /put_ping?name=x (no -d, --max-time 5)"
empty_method PUT "/put_ping?name=x" "empty_put"
assert_body_contains "$_QA_LAST_BODY" 'put x' "empty_put body"

echo "empty_post.test.sh OK"
stop_quackapi
