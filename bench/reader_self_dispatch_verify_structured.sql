-- Small mixed-format companion to the scaled text/BLOB and AST programs.
-- Each per-file SQL statement executes through quackapi_post on this same DB.
SET threads = 4;
SET memory_limit = '4GB';
SET preserve_insertion_order = false;
LOAD json;
LOAD yaml;

CREATE ROUTE reader_dispatch POST '/dispatch' AS SELECT * FROM query($sql);
SELECT * FROM quackapi_serve(getenv('READER_BENCH_PORT')::INTEGER,
    host := '127.0.0.1', worker_threads := 64, access_log := false,
    compression := false, query_timeout_ms := 120000,
    max_response_bytes := 16777216, block := false);
SET curl_httpfs_client_implementation = 'multi_curl';
SELECT curl_httpfs_http_util_name() AS provider;

WITH paths(reader, path) AS (
    VALUES
        ('csv', getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/test/sakila/data/country.csv'),
        ('json', getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/src/storage/version_map.json'),
        ('yaml', getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/api_spec/v1/query/query.yaml')
), source AS (
    SELECT reader, path,
        printf('SELECT * FROM read_%s($path$%s$path$)%s', reader, path,
            CASE WHEN reader = 'csv' THEN ' ORDER BY country_id' ELSE '' END) AS sql
    FROM paths
), sent AS (
    SELECT reader, path,
        quackapi_post('http://127.0.0.1:' || getenv('READER_BENCH_PORT') || '/dispatch',
            json_object('sql', sql)::VARCHAR) AS receipt
    FROM source
), direct AS (
    SELECT 'csv' AS reader, to_json(list(r ORDER BY country_id))::VARCHAR AS body,
        count(country_id) AS rows
    FROM (SELECT * FROM read_csv(getenv('READER_BENCH_SOURCE_ROOT') ||
        '/duckdb/test/sakila/data/country.csv')) r
    UNION ALL
    SELECT 'json', to_json(list(r))::VARCHAR, count(r)
    FROM (SELECT * FROM read_json(getenv('READER_BENCH_SOURCE_ROOT') ||
        '/duckdb/src/storage/version_map.json')) r
    UNION ALL
    SELECT 'yaml', to_json(list(r))::VARCHAR, count(r)
    FROM (SELECT * FROM read_yaml(getenv('READER_BENCH_SOURCE_ROOT') ||
        '/duckdb/api_spec/v1/query/query.yaml')) r
), compared AS (
    SELECT sent.reader, sent.path, sent.receipt.status AS status,
        sent.receipt.error AS error, direct.rows AS direct_rows,
        length(sent.receipt.body) AS result_body_bytes,
        sent.receipt.body = direct.body AS exact_match,
        left(sent.receipt.body, 240) AS body_sample
    FROM sent LEFT JOIN direct USING (reader)
), summary AS (
    SELECT count(reader) AS compared_readers, count(DISTINCT reader) AS distinct_readers,
        count(reader) FILTER (WHERE status IS DISTINCT FROM 200 OR error IS NOT NULL) AS request_failures,
        count(reader) FILTER (WHERE exact_match IS DISTINCT FROM true) AS value_mismatches,
        count(reader) FILTER (WHERE reader = 'csv' AND direct_rows = 109) AS csv_rows_ok,
        count(reader) FILTER (WHERE reader = 'json' AND direct_rows = 1) AS json_rows_ok,
        count(reader) FILTER (WHERE reader = 'yaml' AND direct_rows = 1) AS yaml_rows_ok,
        list(struct_pack(reader := reader, path := path, status := status,
            error := error, direct_rows := direct_rows,
            result_body_bytes := result_body_bytes, exact_match := exact_match,
            body_sample := body_sample) ORDER BY reader) AS receipts
    FROM compared
)
SELECT compared_readers, distinct_readers, request_failures, value_mismatches,
    csv_rows_ok, json_rows_ok, yaml_rows_ok, receipts,
    CASE WHEN compared_readers = 3 AND distinct_readers = 3
        AND request_failures = 0 AND value_mismatches = 0
        AND csv_rows_ok = 1 AND json_rows_ok = 1 AND yaml_rows_ok = 1 THEN 'PASS'
    ELSE error(printf('structured self-dispatch mismatch: compared=%s distinct=%s requests=%s values=%s csv=%s json=%s yaml=%s receipts=%s',
        compared_readers::VARCHAR, distinct_readers::VARCHAR,
        request_failures::VARCHAR, value_mismatches::VARCHAR,
        csv_rows_ok::VARCHAR, json_rows_ok::VARCHAR, yaml_rows_ok::VARCHAR,
        coalesce(receipts::VARCHAR, '<none>'))) END AS verdict
FROM summary;

SELECT * FROM quackapi_stop();
