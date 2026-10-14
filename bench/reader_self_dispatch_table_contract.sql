-- Run with an explicitly loaded QuackAPI build and HTTPUtil provider.
-- The route and table function share this one DuckDB instance.
SET threads = 4;
SET memory_limit = '256MB';
SET preserve_insertion_order = false;
SET quackapi_post_concurrency = 32;
LOAD json;

CREATE SEQUENCE table_post_33;
CREATE ROUTE table_post_echo_33 POST '/table-post-33' AS
SELECT $id::BIGINT AS id, nextval('table_post_33') AS side_effect;

CREATE SEQUENCE table_post_4097;
CREATE ROUTE table_post_echo_4097 POST '/table-post-4097' AS
SELECT $id::BIGINT AS id, nextval('table_post_4097') AS side_effect;

CREATE SEQUENCE table_post_mixed;
CREATE ROUTE table_post_echo_mixed POST '/table-post-mixed' AS
SELECT $id::BIGINT AS id, nextval('table_post_mixed') AS side_effect;

SELECT * FROM quackapi_serve(getenv('READER_BENCH_PORT')::INTEGER,
    host := '127.0.0.1', worker_threads := 64, access_log := false,
    compression := false, query_timeout_ms := 120000,
    max_response_bytes := 16777216, block := false);
SET curl_httpfs_client_implementation = 'multi_curl';
SELECT curl_httpfs_http_util_name() AS provider;

-- The 33rd result is the first row of the final partial batch. Every request
-- must execute once and retain its input row even if completion order differs.
WITH sent AS (
    SELECT ordinal, response
    FROM quackapi_post((
        SELECT i AS ordinal,
            'http://127.0.0.1:' || getenv('READER_BENCH_PORT') || '/table-post-33' AS url,
            json_object('id', i)::VARCHAR AS body
        FROM range(33) t(i)
    ))
), checked AS (
    SELECT ordinal, response.status AS status, response.error AS request_error,
        json_extract_string(response.body::JSON, '$[0].id')::BIGINT AS returned_id,
        json_extract_string(response.body::JSON, '$[0].side_effect')::BIGINT AS side_effect
    FROM sent
), summary AS (
    SELECT count(ordinal) AS rows_seen, count(DISTINCT ordinal) AS distinct_inputs,
        count(ordinal) FILTER (WHERE status IS DISTINCT FROM 200 OR request_error IS NOT NULL) AS failures,
        count(ordinal) FILTER (WHERE returned_id IS DISTINCT FROM ordinal) AS mismatches,
        count(DISTINCT side_effect) AS distinct_effects, min(side_effect) AS first_effect,
        max(side_effect) AS last_effect
    FROM checked
)
SELECT rows_seen, distinct_inputs, failures, mismatches, distinct_effects,
    first_effect, last_effect,
    CASE WHEN rows_seen = 33 AND distinct_inputs = 33 AND failures = 0
        AND mismatches = 0 AND distinct_effects = 33
        AND first_effect = 1 AND last_effect = 33 THEN 'PASS'
    ELSE error('quackapi_post(TABLE) lost, repeated, or misaligned a 33-row request') END AS verdict
FROM summary;

-- This crosses the 2048-row vector boundary as well as 128 batches of 32.
WITH sent AS (
    SELECT ordinal, response
    FROM quackapi_post((
        SELECT i AS ordinal,
            'http://127.0.0.1:' || getenv('READER_BENCH_PORT') || '/table-post-4097' AS url,
            json_object('id', i)::VARCHAR AS body
        FROM range(4097) t(i)
    ))
), checked AS (
    SELECT ordinal, response.status AS status, response.error AS request_error,
        json_extract_string(response.body::JSON, '$[0].id')::BIGINT AS returned_id,
        json_extract_string(response.body::JSON, '$[0].side_effect')::BIGINT AS side_effect
    FROM sent
), summary AS (
    SELECT count(ordinal) AS rows_seen, count(DISTINCT ordinal) AS distinct_inputs,
        count(ordinal) FILTER (WHERE status IS DISTINCT FROM 200 OR request_error IS NOT NULL) AS failures,
        count(ordinal) FILTER (WHERE returned_id IS DISTINCT FROM ordinal) AS mismatches,
        count(DISTINCT side_effect) AS distinct_effects, min(side_effect) AS first_effect,
        max(side_effect) AS last_effect
    FROM checked
)
SELECT rows_seen, distinct_inputs, failures, mismatches, distinct_effects,
    first_effect, last_effect,
    CASE WHEN rows_seen = 4097 AND distinct_inputs = 4097 AND failures = 0
        AND mismatches = 0 AND distinct_effects = 4097
        AND first_effect = 1 AND last_effect = 4097 THEN 'PASS'
    ELSE error('quackapi_post(TABLE) lost, repeated, or misaligned a 4097-row request') END AS verdict
FROM summary;

-- A NULL URL skips the transport; an invalid endpoint produces its own error
-- receipt; three rows with constant URL/body still cause three side effects.
WITH sent AS (
    SELECT ordinal, response
    FROM quackapi_post((
        SELECT i AS ordinal,
            CASE WHEN i = 0 THEN NULL::VARCHAR
                WHEN i = 1 THEN 'http://127.0.0.1:1/'
                ELSE 'http://127.0.0.1:' || getenv('READER_BENCH_PORT') || '/table-post-mixed' END AS url,
            CASE WHEN i = 0 THEN NULL::VARCHAR ELSE '{"id":7}' END AS body
        FROM range(5) t(i)
    ))
), checked AS (
    SELECT ordinal, response, response.status AS status,
        response.error AS request_error,
        CASE WHEN response.status = 200 THEN
            json_extract_string(response.body::JSON, '$[0].id')::BIGINT END AS returned_id,
        CASE WHEN response.status = 200 THEN
            json_extract_string(response.body::JSON, '$[0].side_effect')::BIGINT END AS side_effect
    FROM sent
), summary AS (
    SELECT count(ordinal) AS rows_seen,
        count(ordinal) FILTER (WHERE ordinal = 0 AND response IS NULL) AS null_receipts,
        count(ordinal) FILTER (WHERE ordinal = 1 AND status = 0 AND request_error IS NOT NULL) AS error_receipts,
        count(ordinal) FILTER (WHERE ordinal >= 2 AND status = 200 AND returned_id = 7) AS successes,
        count(DISTINCT side_effect) AS distinct_effects
    FROM checked
)
SELECT rows_seen, null_receipts, error_receipts, successes, distinct_effects,
    CASE WHEN rows_seen = 5 AND null_receipts = 1 AND error_receipts = 1
        AND successes = 3 AND distinct_effects = 3 THEN 'PASS'
    ELSE error('quackapi_post(TABLE) mixed/null/constant row contract failed') END AS verdict
FROM summary;

SELECT sequence_name, last_value FROM duckdb_sequences()
WHERE sequence_name IN ('table_post_33', 'table_post_4097', 'table_post_mixed')
ORDER BY sequence_name;
SELECT * FROM quackapi_stop();
