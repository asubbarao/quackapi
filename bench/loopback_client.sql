-- Does routing plain http:// through HTTPUtil cost latency on loopback?
--
-- quackapi used to send every http:// URL to the vendored duckdb_httplib client
-- instead of HTTPUtil. That branch may have been a deliberate optimisation --
-- the gateway benchmark is entirely 127.0.0.1, where a connection is cheap and
-- per-request client overhead is the whole cost -- or it may have been an
-- oversight. This measures it rather than assuming either way.
--
-- Run against both builds, same machine, same session shape:
--   build A: IsPlainHTTP present (httplib for plain http)   -- commit d7d5be4
--   build B: IsPlainHTTP gone    (HTTPUtil/curl everywhere) -- working tree
-- The number that matters is the delta, not the absolute.

LOAD quackapi;

-- quackapi_serve(port, host := '0.0.0.0', threads := <db threads>, tune := false,
--   write_timeout_sec := 30, read_timeout_sec := 30, compression_min_bytes := 256,
--   pg_dsn := NULL, quack_auth := false)
FROM quackapi_serve(29500, host := '127.0.0.1');

CREATE ROUTE ping GET '/ping' AS SELECT 1 AS ok;

-- Warm the pool: the first request per host pays a dial the rest do not, and a
-- cold-start number would say more about connection setup than about the client.
CREATE OR REPLACE TABLE warmup AS
SELECT quackapi_fetch('http://127.0.0.1:29500/ping') AS response FROM range(50);

SET VARIABLE t0 = current_timestamp;

-- 2000 fetches, fanned out the way a route handler doing upstream calls would be.
CREATE OR REPLACE TABLE measured AS
-- quackapi_fetch(url, headers := MAP{}, stall_ms := 0)
SELECT quackapi_fetch('http://127.0.0.1:29500/ping') AS response FROM range(2000);

CREATE OR REPLACE TABLE loopback_result AS
SELECT
    'plain-http loopback, 2000 fetches' AS measurement,
    age(current_timestamp, getvariable('t0'))  AS elapsed,
    array_agg(DISTINCT response.status)        AS statuses,
    len(statuses)                              AS distinct_statuses,
    array_agg(DISTINCT response.error) AS errors
FROM measured;

FROM loopback_result;

-- Pool behaviour is part of the answer: a client that cannot reuse a loopback
-- connection pays a dial per request, which is where a real difference would show.
-- quackapi_http_pool() takes no arguments.
FROM quackapi_http_pool();

-- quackapi_http_util_name() takes no arguments -- names the active outbound client.
SELECT quackapi_http_util_name() AS active_client;

FROM quackapi_stop(29500);
