-- Outbound fan-out route: one inbound request -> N concurrent outbound API calls.
--
-- Two settings carry this workload; both were measured, not guessed:
--
-- 1. SET GLOBAL force_download=true
--    httpfs normally issues a HEAD to learn content-length, then the GET. Against a
--    static object that probe is cheap; against a DYNAMIC API endpoint it runs the
--    upstream handler TWICE and exactly doubles latency (measured: 204ms vs a 100ms
--    upstream, at n=1, where concurrency plays no part). force_download skips the probe.
--    GLOBAL is required: plain SET is session-scoped and never reaches quackapi's
--    per-thread request Connection, so it silently does nothing.
--
-- 2. SET threads=<fan-out width>
--    read_json parallelizes across DuckDB threads, so concurrency == thread count and
--    latency steps in ceil(n/threads) waves. Each scan thread holds a fixed ~32MiB read
--    buffer, so memory_limit must cover threads*32MiB (64 * 32MiB = 2GB, under 4GB).
INSTALL httpfs; LOAD httpfs;
SET httpfs_client_implementation='curl';
SET lambda_syntax='ENABLE_SINGLE_ARROW';
SET GLOBAL force_download=true;
SET memory_limit='4GB';
SET threads=64;
CREATE ROUTE fanout GET '/fanout' AS
  SELECT count(*) AS n, sum(id) AS sum_ids
  FROM read_json(
    list_transform(range(0,$n::INTEGER),
      x -> 'http://127.0.0.1:9000/slow?ms=' || $ms::VARCHAR || '&id=' || x::VARCHAR
             || '&nz=' || (random()*1e9)::BIGINT::VARCHAR),
    columns={id:'INTEGER', ok:'BOOLEAN', ms:'INTEGER'}, format='unstructured'
  );
