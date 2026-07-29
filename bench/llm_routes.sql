-- quackapi as an LLM API gateway.
--
-- Shape of the workload (the realistic one): inbound request -> call ollama ->
-- durably log the VERBATIM upstream body -> respond. The logging must not block
-- the response, and must not be lost if the process dies.
--
-- quackapi does all three in ONE statement:
--   * http_post          -> the upstream call
--   * quackapi_enqueue   -> durable, broker-less job row (single file, WAL-backed)
--   * SELECT             -> the client response
-- A separate drainer (llm_drain.sql) ships jobs into the pgEdge verbatim sink.
--
-- Note: pg INSERT ... RETURNING is not supported through the DuckDB postgres
-- extension, so writing straight to pgEdge inside the handler cannot also return
-- a body. The queue is the correct answer anyway -- it keeps Postgres off the
-- response path entirely.
INSTALL http_client FROM community; LOAD http_client;
SET GLOBAL force_download=true;   -- see fanout_routes.sql: httpfs HEAD probe
SET memory_limit='4GB';
SET threads=32;

CREATE OR REPLACE QUEUE llm_log WITH (max_attempts=5, visibility_timeout='60s');

-- Embeddings: short upstream service time, so framework overhead is a large and
-- clearly visible share of end-to-end latency.
CREATE ROUTE embed POST '/llm/embed' AS
  WITH r AS (
    SELECT json_extract_string((http_post(
      'http://127.0.0.1:11434/api/embeddings',
      MAP{'Content-Type':'application/json'},
      to_json({model: $model::VARCHAR, prompt: $prompt::VARCHAR})
    )).body, '$') AS j
  )
  SELECT
    json_array_length(json_extract(j, '$.embedding'))              AS dims,
    quackapi_enqueue('llm_log', json_object(
      'stack',   'quackapi',
      'api',     'embeddings',
      'model',   $model::VARCHAR,
      'request', json_object('model', $model::VARCHAR, 'prompt', $prompt::VARCHAR),
      'raw',     j
    ))                                                             AS job_id
  FROM r;

-- Generation: real token production. num_predict is pinned so the comparison is
-- not dominated by one stack happening to draw a longer completion.
CREATE ROUTE ask POST '/llm/ask' AS
  WITH r AS (
    SELECT json_extract_string((http_post(
      'http://127.0.0.1:11434/api/generate',
      MAP{'Content-Type':'application/json'},
      to_json({model: $model::VARCHAR, prompt: $prompt::VARCHAR, stream: false,
               options: {num_predict: $num_predict::INTEGER}})
    )).body, '$') AS j
  )
  SELECT
    json_extract_string(j, '$.response')                           AS response,
    (json_extract(j, '$.total_duration')::BIGINT)/1e6              AS ollama_total_ms,
    json_extract(j, '$.eval_count')::INTEGER                       AS out_tokens,
    quackapi_enqueue('llm_log', json_object(
      'stack',   'quackapi',
      'api',     'generate',
      'model',   $model::VARCHAR,
      'request', json_object('model', $model::VARCHAR, 'prompt', $prompt::VARCHAR),
      'raw',     j
    ))                                                             AS job_id
  FROM r;
