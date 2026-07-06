-- ============================================================================
-- compose_realtime.sql — RECEIPTS 13 & 14: redis + radio, the real-time slot.
--
-- R13 `redis` (~5 MB): cache/session-store endpoints. The redis-py slot that
--     virtually every production FastAPI app carries. State lives in Redis, so
--     it survives process death — the two-phase receipt harness proves this by
--     accident: PUT happens in one process, GET reads it back from another.
-- R14 `radio` (~10 MB): live Redis pub/sub INBOUND. The subscription buffers
--     messages from a live channel, queryable via radio_received_messages().
--     Narrows edges.md #2/#3 (SSE/WebSockets) from "no real-time story" to
--     "inbound browser sockets need a relay" — radio is a WebSocket/pub-sub
--     CLIENT, not a server. Honest. OUTBOUND is receipt-blocked: see gotchas.
--
-- Separate file from compose.sql because it REQUIRES a local Redis on :6379
-- (`brew services start redis`). Load: framework.sql -> compose.sql -> this.
--
-- Gotchas learned building this (all observed, duckdb v1.5.3):
--   * redis secret MUST include `password ''` even for passwordless local
--     Redis — the extension requires host+port+password all present and
--     reports the miss as a misleading "Redis secret not found".
--   * radio URL scheme is `redis-tcp://host:port?channel=NAME` (the `redis-`
--     prefix is stripped and the rest handed to redis-plus-plus).
--   * radio TRANSMIT over redis is BROKEN in the published osx_arm64 build:
--     any radio_transmit_message with an active redis subscription ABORTS the
--     whole process asynchronously (std::bad_optional_access in the delivery
--     thread); transmitting without a subscription is refused. Outbound
--     messaging here uses the redis ext's lpush (queue semantics) instead.
--   * radio_subscriptions() throws an INTERNAL vector-type error (UINT64 vs
--     INT64) — extension bug; do not use it in handlers.
--   * redis_lpush returns the raw RESP integer (':1'), not a parsed value.
--   * subscribing is async: publishes racing a fresh subscribe can be lost
--     (Redis pub/sub has no replay). Receipts wait after boot.
-- ============================================================================
INSTALL redis FROM community;  LOAD redis;
INSTALL radio FROM community;  LOAD radio;

CREATE SECRET IF NOT EXISTS redis (
  TYPE redis, PROVIDER config,
  host 'localhost', port '6379', password ''
);

-- One subscription per session; every session that boots this file can both
-- hear and speak on the channel.
CALL radio_subscribe('redis-tcp://localhost:6379?channel=qapi_events');

DELETE FROM param_schema WHERE starts_with(route_id, 'composert_');
DELETE FROM routes WHERE starts_with(route_id, 'composert_');

-- ----------------------------------------------------------------------------
-- RECEIPT 13 — redis: cache / session-store endpoints.
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'composert_cache_put', 'POST', '/cache',
  'SELECT to_json({key: {key}, result: redis_set(''qapi:cache:'' || {key}, {value}, ''redis'')}) AS body',
  'dynamic', 'Write a value to the Redis-backed cache (redis ext)', 200);

INSERT INTO routes SELECT * FROM register_route(
  'composert_cache_get', 'GET', '/cache/{key}',
  'SELECT to_json({key: {key}, found: redis_exists(''qapi:cache:'' || {key}, ''redis''), value: redis_get(''qapi:cache:'' || {key}, ''redis'')}) AS body',
  'dynamic', 'Read a value from the Redis-backed cache (redis ext)', 200);

-- ----------------------------------------------------------------------------
-- RECEIPT 13b — redis lists as a task queue: the broker-push slot.
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'composert_queue_push', 'POST', '/queue/push',
  'SELECT to_json({queued: {task}, resp: redis_lpush(''qapi:queue:tasks'', {task}, ''redis'')}) AS body',
  'dynamic', 'Push a task onto the Redis-backed work queue (redis ext)', 201);

-- ----------------------------------------------------------------------------
-- RECEIPT 14 — radio: read the live pub/sub subscription buffer.
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'composert_recent', 'GET', '/events/recent',
  'SELECT coalesce(json_group_array(to_json({type: r.message_type, message: r.message::VARCHAR, received_at: strftime(r.receive_time, ''%H:%M:%S'')})), ''[]'') AS body FROM radio_received_messages() r',
  'dynamic', 'Read buffered messages received on the subscription (radio ext)', 200);

INSERT INTO param_schema (route_id, name, location, type, required, constraint_json)
SELECT 'composert_cache_put', 'key',     'body', 'string', true, NULL UNION ALL
SELECT 'composert_cache_put', 'value',   'body', 'string', true, NULL UNION ALL
SELECT 'composert_cache_get',  'key',   'path', 'string', true, NULL UNION ALL
SELECT 'composert_queue_push', 'task',  'body', 'string', true, NULL;
