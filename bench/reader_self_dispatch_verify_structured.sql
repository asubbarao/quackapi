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
)
SELECT sent.reader, sent.path, sent.receipt.status AS status,
    sent.receipt.error AS error, direct.rows AS direct_rows,
    length(sent.receipt.body) AS result_body_bytes,
    sent.receipt.body = direct.body AS exact_match
FROM sent JOIN direct USING (reader)
ORDER BY reader;

SELECT * FROM quackapi_stop();
