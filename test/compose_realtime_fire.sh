#!/usr/bin/env bash
# compose_realtime_fire.sh — prove the radio receipt LIVE (edges.md #2/#3 narrowing).
# One session subscribed to qapi_events; an EXTERNAL redis-cli publisher fires
# twice on the channel while the session waits; the /events/recent handler SQL
# then dumps the buffer — live inbound events observed inside the engine.
#
# NOT tested here: radio OUTBOUND transmit — the published osx_arm64 build
# aborts the process (std::bad_optional_access) on any redis transmit with an
# active subscription. See the gotcha ledger in compose_realtime.sql.
set -uo pipefail
cd "$(dirname "$0")/.."

# Publisher waits WELL past stack boot (.read of framework+compose takes several
# seconds) — Redis pub/sub has no replay, so anything published before the
# subscription connects is silently lost (observed: publishes at t+4s -> []).
( sleep 12; redis-cli publish qapi_events 'deploy finished' >/dev/null
  sleep 1; redis-cli publish qapi_events 'cache invalidated: user-7' >/dev/null ) &

duckdb -unsigned <<'SQL'
.read framework.sql
.read compose.sql
.read compose_realtime.sql
-- wait past the async subscribe, through both external publishes
SELECT length(content) AS publish_window FROM read_text('sleep 16; echo ok |');
-- phase-2 of GET /events/recent
SELECT '~~LIVE~~' || coalesce(json_group_array(to_json({type: r.message_type, message: r.message::VARCHAR})), '[]') AS proof
FROM radio_received_messages() r;
SQL
wait
