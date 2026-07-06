#!/usr/bin/env bash
# compose_cron_fire.sh — prove cronjob receipts actually FIRE (edges.md #4 defeat).
# One live session: register the heartbeat route's job via handle_request, hold the
# session open ~35s, then count heartbeat rows. >=2 rows == background execution
# happened AFTER the "response" — fire-and-forget inside the engine.
set -uo pipefail
cd "$(dirname "$0")/.."
duckdb -unsigned <<'SQL'
.read framework.sql
.read compose.sql
-- register the job exactly as the API would (phase-2 of POST /jobs/heartbeat)
SELECT cron('INSERT INTO compose_heartbeats SELECT now()', '*/10 * * * * *') AS scheduled;
-- hold the session open while the scheduler runs (sleep via shellfs, no python)
SELECT length(content) AS slept_marker FROM read_text('sleep 35; echo done |');
SELECT '~~FIRED~~' || array_length(array_agg(ts)) || ' heartbeats: ' || array_to_string(array_agg(strftime(ts, '%H:%M:%S') ORDER BY ts), ', ') AS proof
FROM compose_heartbeats;
SQL
