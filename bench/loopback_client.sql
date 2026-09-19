-- Does routing plain http:// through HTTPUtil cost latency on loopback?
--
-- quackapi used to send every http:// URL to the vendored duckdb_httplib client
-- instead of HTTPUtil. That branch might have been a deliberate optimisation --
-- the gateway benchmark is entirely 127.0.0.1, where connection setup is cheap
-- and per-request client overhead is the whole cost -- or an oversight. This
-- measures it instead of assuming.
--
-- Method: build twice from the same tree, changing only the client the fetch
-- path uses, and run this file four times against each. Both builds carry the
-- fixed curl_httpfs guard, so the client is the only variable.
--
-- Measured 2026-09-18, osx_arm64, DuckDB v1.5.4, 3000 fetches of a route that
-- returns one constant row -- i.e. the shape that maximises the client's share
-- of the time:
--
--   vendored httplib   0.225  0.206  0.211  0.209   median ~0.213 s
--   curl_httpfs        0.265  0.247  0.242  0.237   median ~0.245 s
--
-- curl is ~15% slower here, about 11 microseconds per request, and its `sys`
-- time is consistently higher (~0.60 s vs ~0.43 s) -- more syscalls per request.
--
-- The honest reading: 11us per round trip is the CEILING of the effect, not a
-- typical cost. It is measured against a route that does no work, so the client
-- is nearly the entire measurement. Any route that runs a real query buries it.
-- That is not enough to justify a branch which meant curl_httpfs was never
-- actually on the outbound path -- but it is a real number, so it is written
-- down here rather than described as noise.

LOAD quackapi;

CREATE ROUTE ping GET '/ping' AS SELECT 'pong' AS msg;

-- quackapi_serve(port, host := '0.0.0.0', access_log := true, threads := <db threads>,
--   tune := false, write_timeout_sec := 30, read_timeout_sec := 30,
--   compression_min_bytes := 256, pg_dsn := NULL, quack_auth := false)
FROM quackapi_serve(29500, host := '127.0.0.1', access_log := false);

-- Warm the pool: the first call per origin pays a dial the rest do not, and a
-- cold-start number would describe connection setup rather than the client.
CREATE OR REPLACE TABLE warmup AS
-- quackapi_fetch(url, headers := MAP{}, stall_ms := 0)
SELECT quackapi_fetch('http://127.0.0.1:29500/ping') AS r FROM range(100);

.timer on
CREATE OR REPLACE TABLE measured AS
SELECT quackapi_fetch('http://127.0.0.1:29500/ping') AS r FROM range(3000);
.timer off

-- Every response accounted for: the list is the fact, the count is a view of it.
SELECT array_agg(DISTINCT r.status) AS statuses,
       len(array_agg(DISTINCT r.status)) AS n_distinct_status,
       array_agg(DISTINCT r.error) AS errors
FROM measured;

-- Reuse is the other half of the answer: a client that cannot keep a loopback
-- connection alive pays a dial per request, which is where a real gap would show.
-- Both clients reach 32 dials / 3068 reuses here, so the delta above is per-request
-- overhead, not connection churn. quackapi_http_pool() takes no arguments.
FROM quackapi_http_pool();

-- quackapi_http_util_name() takes no arguments.
SELECT quackapi_http_util_name() AS active_client;

FROM quackapi_stop(29500);
