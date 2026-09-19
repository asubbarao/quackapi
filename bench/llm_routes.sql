-- quackapi as an LLM API gateway.
--
-- Shape of the workload (the realistic one): inbound request -> call ollama ->
-- durably log the VERBATIM upstream body -> respond. The logging must not block
-- the response, and must not be lost if the process dies.
--
-- quackapi does all three in ONE statement:
--   * quackapi_post       -> the upstream call
--   * quackapi_enqueue    -> durable, broker-less job row (single file, WAL-backed)
--   * SELECT              -> the client response
-- A separate drainer (llm_drain.sql) ships jobs into the pgEdge verbatim sink.
--
-- Was the community `http_client` extension's http_post(). That extension dials
-- a brand-new httplib::Client (fresh TCP+TLS handshake) on every call -- confirmed
-- by reading query-farm/httpclient's SetupHttpClient(), and it exposes no pooling
-- setting to fix that. curl_httpfs does not help either: it only replaces DuckDB's
-- own httpfs file-IO client (read_text/read_csv/read_parquet), not a POST-capable
-- extension. quackapi_post() is quackapi's own function and already has the pooled
-- outbound HTTP path from f3478d5 -- same .body accessor, so this is a drop-in
-- swap. See bench/LLM_GATEWAY.md for the before/after numbers.
--
-- Note: pg INSERT ... RETURNING is not supported through the DuckDB postgres
-- extension, so writing straight to pgEdge inside the handler cannot also return
-- a body. The queue is the correct answer anyway -- it keeps Postgres off the
-- response path entirely.
LOAD httpfs;
SET GLOBAL force_download=true;   -- see fanout_routes.sql: httpfs HEAD probe
SET memory_limit='4GB';
-- SET threads=32 used to sit here. It is the DuckDB query budget, and pinning it
-- to the same 32 as the HTTP worker budget made the two fight: every in-flight
-- handler holds a worker thread AND quackapi_enqueue opens a nested connection
-- that wants query capacity of its own, so the ask at 32 concurrent requests was
-- twice what was on offer. The two are different resources. Leave the query
-- budget at DuckDB's default (all cores) and size the HTTP side below.

-- IMPORTANT — serve this file with worker_threads WELL ABOVE peak concurrency:
--   SELECT * FROM quackapi_serve(8000, worker_threads := 128);
-- Both routes below block a worker thread for the FULL duration of the upstream
-- call. At the default worker_threads=32, concurrency past 32 does not degrade
-- gracefully -- throughput collapses (303 -> 68 rps) and the latency tail runs to
-- 28s, while the median stays a deceptively healthy ~104ms. With an upstream as
-- slow as ollama generate (~1.2s), a 32-worker pool starves almost immediately.
-- Raising it to 128 removes the cliff completely: ~0ms overhead over the upstream's
-- own service time. Full measurements: bench/LLM_GATEWAY.md.
-- worker_threads is the only HTTP dial: the pending queue follows it, and past
-- both the server answers 503 rather than dropping the connection.

CREATE OR REPLACE QUEUE llm_log WITH (max_attempts=5, visibility_timeout='60s');

-- Embeddings: short upstream service time, so framework overhead is a large and
-- clearly visible share of end-to-end latency.
CREATE ROUTE embed POST '/llm/embed' AS
  WITH r AS (
    SELECT quackapi_post(
      'http://127.0.0.1:11434/api/embeddings',
      to_json({model: $model::VARCHAR, prompt: $prompt::VARCHAR})
    ) AS upstream
  ), parsed AS (
    SELECT upstream, json_extract_string(upstream.body, '$') AS j
    FROM r
  )
  SELECT
    CASE WHEN upstream.status BETWEEN 200 AND 299
              AND json_valid(upstream.body)
              AND json_type(j, '$.embedding') = 'ARRAY'
              AND json_array_length(json_extract(j, '$.embedding')) > 0
         THEN json_array_length(json_extract(j, '$.embedding'))
         ELSE error('ollama embedding response is invalid') END     AS dims,
    CASE WHEN upstream.status BETWEEN 200 AND 299
              AND json_valid(upstream.body)
              AND json_type(j, '$.embedding') = 'ARRAY'
              AND json_array_length(json_extract(j, '$.embedding')) > 0
         THEN quackapi_enqueue('llm_log', json_object(
      'stack',   'quackapi',
      'api',     'embeddings',
      'model',   $model::VARCHAR,
      'request', json_object('model', $model::VARCHAR, 'prompt', $prompt::VARCHAR),
      'raw',     j
    )) ELSE error('ollama embedding response is invalid') END       AS job_id
  FROM parsed;

-- Generation: real token production. num_predict is pinned so the comparison is
-- not dominated by one stack happening to draw a longer completion.
CREATE ROUTE ask POST '/llm/ask' AS
  WITH r AS (
    SELECT quackapi_post(
      'http://127.0.0.1:11434/api/generate',
      to_json({model: $model::VARCHAR, prompt: $prompt::VARCHAR, stream: false,
               options: {num_predict: $num_predict::INTEGER}})
    ) AS upstream
  ), parsed AS (
    SELECT upstream, json_extract_string(upstream.body, '$') AS j
    FROM r
  )
  SELECT
    CASE WHEN upstream.status BETWEEN 200 AND 299
              AND json_valid(upstream.body)
              AND json_type(j, '$.response') = 'VARCHAR'
              AND try_cast(json_extract(j, '$.total_duration') AS BIGINT) > 0
         THEN json_extract_string(j, '$.response')
         ELSE error('ollama generation response is invalid') END     AS response,
    CASE WHEN upstream.status BETWEEN 200 AND 299
              AND json_valid(upstream.body)
              AND json_type(j, '$.response') = 'VARCHAR'
              AND try_cast(json_extract(j, '$.total_duration') AS BIGINT) > 0
         THEN (json_extract(j, '$.total_duration')::BIGINT)/1e6
         ELSE error('ollama generation response is invalid') END     AS ollama_total_ms,
    try_cast(json_extract(j, '$.eval_count') AS INTEGER)             AS out_tokens,
    CASE WHEN upstream.status BETWEEN 200 AND 299
              AND json_valid(upstream.body)
              AND json_type(j, '$.response') = 'VARCHAR'
              AND try_cast(json_extract(j, '$.total_duration') AS BIGINT) > 0
         THEN quackapi_enqueue('llm_log', json_object(
      'stack',   'quackapi',
      'api',     'generate',
      'model',   $model::VARCHAR,
      'request', json_object('model', $model::VARCHAR, 'prompt', $prompt::VARCHAR),
      'raw',     j
    )) ELSE error('ollama generation response is invalid') END       AS job_id
  FROM parsed;
