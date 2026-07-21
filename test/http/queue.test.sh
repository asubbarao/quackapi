#!/usr/bin/env bash
# HTTP: enqueue-from-a-route, worker drains via quackapi_dequeue+ack, GET shows result.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$DIR/lib.sh"

PORT="${QUACKAPI_TEST_PORT:-18990}"
INIT="$(mktemp /tmp/quackapi_queue_http_XXXXXX.sql)"

cat >"$INIT" <<'SQL'
CREATE QUEUE default WITH (max_attempts=3, visibility_timeout='30s');

CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;

CREATE ROUTE enqueue POST '/jobs' STATUS 201 PARAM payload VARCHAR
  AS SELECT quackapi_enqueue('default', $payload::VARCHAR) AS job_id;

-- Worker tick: claim up to 10 jobs and ack each (compose as one SELECT).
-- DuckDB CTEs cannot host INSERT; ack is a scalar side-effect in the projection.
CREATE ROUTE drain POST '/drain' AS
  SELECT id AS job_id,
         'processed:' || payload AS result,
         quackapi_ack('default', id) AS acked
  FROM quackapi_dequeue('default', 10);

CREATE ROUTE results GET '/results' AS
  SELECT id AS job_id, payload, status FROM quackapi_jobs WHERE status = 'done' ORDER BY id;

CREATE ROUTE stats GET '/stats' AS
  SELECT name, depth, in_flight, dead FROM quackapi_queues();
SQL

boot_quackapi "$PORT" "$INIT"
rm -f "$INIT"

echo "-- 1. health"
curl_json GET "/health"
assert_status "$_QA_LAST_STATUS" "200" "health"
assert_body_contains "$_QA_LAST_BODY" '"status":"ok"' "health body"

echo "-- 2. POST /jobs enqueue"
curl_json POST "/jobs" -H 'Content-Type: application/json' -d '{"payload":"{\"task\":\"email\"}"}'
assert_status "$_QA_LAST_STATUS" "201" "enqueue"
assert_body_contains "$_QA_LAST_BODY" '"job_id"' "enqueue job_id"
JOB_ID="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])[0]['job_id'])" "$_QA_LAST_BODY")"
echo "  job_id=$JOB_ID"

echo "-- 3. stats before drain (depth>=1)"
curl_json GET "/stats"
assert_status "$_QA_LAST_STATUS" "200" "stats_before"
assert_body_contains "$_QA_LAST_BODY" '"depth":1' "depth before"

echo "-- 4. POST /drain worker"
# Vendored httplib (third_party/httplib) treats empty bodies per RFC 7230 §3.3.3
# when neither Content-Length nor Transfer-Encoding is present — genuine empty POST.
curl_json POST "/drain"
assert_status "$_QA_LAST_STATUS" "200" "drain"
assert_body_contains "$_QA_LAST_BODY" "\"job_id\":${JOB_ID}" "drain job_id"
assert_body_contains "$_QA_LAST_BODY" '"acked":true' "drain acked"
assert_body_contains "$_QA_LAST_BODY" 'processed:' "drain result"

echo "-- 5. GET /results shows done job"
curl_json GET "/results"
assert_status "$_QA_LAST_STATUS" "200" "results"
assert_body_contains "$_QA_LAST_BODY" "\"job_id\":${JOB_ID}" "result job_id"
assert_body_contains "$_QA_LAST_BODY" '"status":"done"' "result status"

echo "-- 6. stats after drain (depth=0)"
curl_json GET "/stats"
assert_status "$_QA_LAST_STATUS" "200" "stats_after"
assert_body_contains "$_QA_LAST_BODY" '"depth":0' "depth after"

echo "queue.test.sh OK"
stop_quackapi
