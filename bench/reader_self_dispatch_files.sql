-- Full-response, row-driven file-reader workload. This same DuckDB process
-- starts the listener, sends each POST to itself, and retains the complete
-- response body before reducing it. The timing query parses the JSON array
-- length; verify_files.sql compares every content value to direct readers.
-- READER_BENCH_PORT is an isolated port. READER_BENCH_SOURCE_ROOT is the
-- checkout containing the pinned DuckDB submodule (069cc9f9).
SET threads = 4;
SET memory_limit = '4GB';
SET preserve_insertion_order = false;
LOAD hostfs;
LOAD json;

CREATE ROUTE reader_dispatch POST '/dispatch' AS
SELECT * FROM query($sql);
SELECT * FROM quackapi_serve(
    getenv('READER_BENCH_PORT')::INTEGER,
    host := '127.0.0.1',
    worker_threads := 64,
    access_log := false,
    compression := false,
    query_timeout_ms := 120000,
    max_response_bytes := 16777216,
    block := false
);
SET curl_httpfs_client_implementation = 'multi_curl';
SELECT curl_httpfs_http_util_name() AS provider,
    current_setting('threads') AS threads,
    current_setting('memory_limit') AS memory_limit;
PRAGMA enable_profiling = 'json';
SET profiling_output = getenv('READER_BENCH_PROFILE');

WITH candidates AS (
    SELECT file FROM glob(getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/src/**/*')
    UNION ALL SELECT file FROM glob(getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/test/**/*')
    UNION ALL SELECT file FROM glob(getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/tools/**/*')
    UNION ALL SELECT file FROM glob(getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/extension/**/*')
), eligible AS (
    SELECT file AS path, file_size(file) AS source_bytes
    FROM candidates
    WHERE list_contains([ends_with(file, '.cpp'), ends_with(file, '.hpp'),
        ends_with(file, '.h'), ends_with(file, '.cc'), ends_with(file, '.c'),
        ends_with(file, '.py'), ends_with(file, '.sql'), ends_with(file, '.md'),
        ends_with(file, '.json'), ends_with(file, '.yaml'),
        ends_with(file, '.yml'), ends_with(file, '.csv')], true)
      AND NOT contains(file, '/third_party/')
      AND file_size(file) BETWEEN 1 AND 131072
), corpus_paths AS (
    SELECT path, source_bytes FROM eligible ORDER BY path LIMIT 4097
), corpus AS (
    SELECT row_number() OVER (ORDER BY path) AS ordinal, path, source_bytes
    FROM corpus_paths
), statements AS (
    SELECT ordinal, path, source_bytes,
        CASE WHEN ordinal % 4 = 0 THEN 'blob' ELSE 'text' END AS reader,
        printf('SELECT filename, content FROM read_%s(%s)',
            CASE WHEN ordinal % 4 = 0 THEN 'blob' ELSE 'text' END,
            '$path$' || path || '$path$') AS sql
    FROM corpus
), sent AS (
    SELECT ordinal, path, source_bytes, reader,
        quackapi_post('http://127.0.0.1:' || getenv('READER_BENCH_PORT') || '/dispatch',
            json_object('sql', sql)::VARCHAR) AS receipt
    FROM statements
), checked AS (
    SELECT ordinal, path, source_bytes, reader, receipt.status AS status,
        receipt.error AS error, receipt.body AS body,
        length(receipt.body) AS body_bytes,
        CASE WHEN receipt.status = 200 THEN json_array_length(receipt.body::JSON) ELSE 0 END AS response_rows
    FROM sent
)
SELECT count(DISTINCT path) AS distinct_files, sum(source_bytes) AS source_bytes,
    count(path) FILTER (WHERE reader = 'text') AS text_posts,
    count(path) FILTER (WHERE reader = 'blob') AS blob_posts,
    sum(response_rows) AS result_rows, sum(body_bytes) AS result_body_bytes,
    count(path) FILTER (WHERE status <> 200) AS bad_statuses,
    count(path) FILTER (WHERE error IS NOT NULL) AS provider_errors,
    min(path) FILTER (WHERE status <> 200 OR error IS NOT NULL) AS first_failed_path,
    min(error) FILTER (WHERE error IS NOT NULL) AS first_error
FROM checked;

PRAGMA disable_profiling;
SELECT latency AS query_seconds, cpu_time AS cpu_seconds,
    system_peak_buffer_memory AS peak_buffer_bytes,
    system_peak_temp_dir_size AS peak_temp_bytes
FROM read_json_auto(getenv('READER_BENCH_PROFILE'));
SELECT * FROM quackapi_stop();
