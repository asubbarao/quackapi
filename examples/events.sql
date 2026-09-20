-- Per-request observability that survives the process.
--
-- quackapi answers every request on a DuckDB connection of its own, so that
-- connection's query and its transaction ARE the request. The `events`
-- extension reports both to a program quackapi never links, one JSON object
-- per event on that program's stdin.
--
-- What arrives is query and transaction lifecycle, and nothing else. There is
-- no row event, no table event and no CDC here. `transaction_rollback` is the
-- reason to run this at all: it is the one account of a request that the
-- in-process span path cannot give you, because the failure and the reporter
-- are the same process.
--
-- Run (sequential session):
--   INSTALL events FROM community;   -- once; quackapi LOADs it, never installs
--   duckdb < examples/events.sql

LOAD quackapi;

-- ---------------------------------------------------------------------------
-- 1. The handler. A command line, spawned once per event, JSON on its stdin.
--    `sh -c 'cat >> …'` appends the bytes unchanged: this file is the raw
--    layer and nothing below ever rewrites it.
-- ---------------------------------------------------------------------------
SET quackapi_events = '/bin/sh -c "cat >> /tmp/quackapi-events.jsonl"';

-- The lifecycle a request is made of. connection_opened is not in this list:
-- events fires it before DuckDB assigns the connection id, so it arrives as
-- 18446744073709551615 and correlates with nothing.
SET quackapi_events_types = [
  'query_begin', 'query_end',
  'transaction_begin', 'transaction_commit', 'transaction_rollback'
];

-- false (the default) puts the handler on the query's critical path and waits
-- for it. true is fire-and-forget: lower latency, no delivery guarantee of any
-- kind — events are dropped silently and you will not know.
SET quackapi_events_async = false;

-- ---------------------------------------------------------------------------
-- 2. Apply it and say where it stands. quackapi moves events_destination /
--    events_types / events_async GLOBALLY, because a plain SET is
--    session-local and a request runs on a connection you never touched.
--    state = 'serving' means the sink is live; 'unavailable' names what is
--    missing. quackapi_serve turns 'unavailable' into a refusal to serve.
-- ---------------------------------------------------------------------------
-- quackapi_events() — no parameters.
SELECT destination, types, async, state, detail FROM quackapi_events();

-- ---------------------------------------------------------------------------
-- 3. Routes. /obs hands the caller the two keys it needs to find its own
--    events; /add rolls back on the second call.
-- ---------------------------------------------------------------------------
CREATE OR REPLACE TABLE ledger(id INTEGER PRIMARY KEY);

CREATE OR REPLACE ROUTE obs GET '/obs' AS
SELECT connection_id, transaction_id, session_name FROM quackapi_events();

CREATE OR REPLACE ROUTE add POST '/add' AS
INSERT INTO ledger VALUES (1) RETURNING id;

-- ---------------------------------------------------------------------------
-- 4. Three requests: one that reports its own keys, one that commits, one that
--    rolls back. session_name is the request id — quackapi names the request's
--    connection with it before the handler runs, so a request that fails (and
--    is told nothing but 500) is still findable in the sink.
--
-- quackapi_request(method, path, body := NULL, headers := NULL,
--                  pg_dsn := NULL (default), query_timeout_ms := NULL
--                  (default: the serve value), max_response_bytes := NULL
--                  (default: the serve value))
-- ---------------------------------------------------------------------------
SELECT status, decode(body) AS body
FROM quackapi_request('GET', '/obs', headers := MAP {'X-Request-ID': 'example-obs'});

SELECT status, decode(body) AS body
FROM quackapi_request('POST', '/add', headers := MAP {'X-Request-ID': 'example-commit'});

SELECT status, decode(body) AS body
FROM quackapi_request('POST', '/add', headers := MAP {'X-Request-ID': 'example-rollback'});

-- ---------------------------------------------------------------------------
-- 5. The raw layer, read whole. An unmaterialized view over the file the
--    handler wrote; the events extension's own field names are the schema.
--
-- read_json(path, format := 'newline_delimited', sample_size := -1,
--           auto_detect := true (default), columns := NULL (default:
--           inferred), records := 'auto' (default), maximum_object_size :=
--           16777216 (default), ignore_errors := false (default),
--           union_by_name := false (default), filename := false (default),
--           hive_partitioning := false (default), maximum_depth := -1
--           (default), dateformat := 'iso' (default), timestampformat :=
--           'iso' (default), compression := 'auto' (default))
--
-- sample_size := -1 reads every record before fixing the schema. The default
-- 20480 would decide off a prefix that has_error does not exist and drop it
-- from every later row.
-- ---------------------------------------------------------------------------
CREATE OR REPLACE VIEW raw_events AS
SELECT * FROM read_json('/tmp/quackapi-events.jsonl', format := 'newline_delimited', sample_size := -1);

DESCRIBE raw_events;

-- ---------------------------------------------------------------------------
-- 6. The correlation. One row per (request, connection, transaction), with the
--    events themselves kept — the list is the fact, the number is a view of
--    the list.
-- ---------------------------------------------------------------------------
SELECT session_name,
       connection_id,
       transaction_id,
       list_sort(array_agg(event)) AS events,
       len(events) AS n,
       array_agg(DISTINCT has_error) AS error_flags,
       array_agg(DISTINCT error_message) AS error_messages
FROM raw_events
WHERE session_name IN ('example-obs', 'example-commit', 'example-rollback')
GROUP BY session_name, connection_id, transaction_id
ORDER BY session_name, connection_id, transaction_id;
