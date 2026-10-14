-- Isolates HTTP transport/materialization from JSON parsing. The full AST
-- bodies still traverse quackapi_post, but only their lengths are reduced.
-- READER_BENCH_MEMORY selects a bounded DuckDB memory limit for this probe.
SET threads = 4;
SET memory_limit = getenv('READER_BENCH_MEMORY');
SET preserve_insertion_order = false;
LOAD hostfs;
LOAD json;
LOAD sitting_duck;

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

WITH eligible AS (
    SELECT file AS path, file_size(file) AS source_bytes
    FROM glob(getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/src/**/*')
    WHERE list_contains([ends_with(file, '.cpp'), ends_with(file, '.hpp'),
        ends_with(file, '.h'), ends_with(file, '.cc'),
        ends_with(file, '.c')], true)
      AND file_size(file) BETWEEN 1024 AND 32768
), corpus_paths AS (
    SELECT path, source_bytes FROM eligible ORDER BY path LIMIT 256
), corpus AS (
    SELECT row_number() OVER (ORDER BY path) AS ordinal, path, source_bytes
    FROM corpus_paths
), statements AS (
    SELECT ordinal, path, source_bytes,
        printf('SELECT * FROM read_ast(%s)', '$path$' || path || '$path$') AS sql
    FROM corpus
), sent AS (
    SELECT ordinal, path, source_bytes,
        quackapi_post('http://127.0.0.1:' || getenv('READER_BENCH_PORT') || '/dispatch',
            json_object('sql', sql)::VARCHAR) AS receipt
    FROM statements
), checked AS (
    SELECT ordinal, path, source_bytes, receipt.status AS status,
        receipt.error AS error, receipt.body AS body,
        length(receipt.body) AS body_bytes
    FROM sent
)
SELECT count(DISTINCT path) AS distinct_ast_files, sum(source_bytes) AS source_bytes,
    sum(body_bytes) AS result_body_bytes,
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
