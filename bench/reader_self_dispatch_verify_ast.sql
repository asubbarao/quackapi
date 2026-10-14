-- Compare every full AST response body, including nested STRUCT/LIST and enum
-- values, to direct read_ast serialization for the same 256 source files.
SET threads = 4;
SET memory_limit = '8GB';
SET preserve_insertion_order = false;
LOAD hostfs;
LOAD json;
LOAD sitting_duck;

CREATE ROUTE reader_dispatch POST '/dispatch' AS SELECT * FROM query($sql);
SELECT * FROM quackapi_serve(getenv('READER_BENCH_PORT')::INTEGER,
    host := '127.0.0.1', worker_threads := 64, access_log := false,
    compression := false, query_timeout_ms := 120000,
    max_response_bytes := 16777216, block := false);
SET curl_httpfs_client_implementation = 'multi_curl';
SELECT curl_httpfs_http_util_name() AS provider;

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
    SELECT row_number() OVER (ORDER BY path) AS ordinal, path, source_bytes FROM corpus_paths
), statements AS (
    SELECT ordinal, path, source_bytes,
        printf('SELECT * FROM read_ast(%s)', '$path$' || path || '$path$') AS sql
    FROM corpus
), sent AS (
    SELECT ordinal, path, source_bytes,
        quackapi_post('http://127.0.0.1:' || getenv('READER_BENCH_PORT') || '/dispatch',
            json_object('sql', sql)::VARCHAR) AS receipt
    FROM statements
), direct AS (
    SELECT file_path AS path, to_json(list(r ORDER BY node_id))::VARCHAR AS body,
        count(node_id) AS ast_rows
    FROM read_ast([
        getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/src/catalog/**/*.cpp',
        getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/src/common/**/*.cpp',
        getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/src/execution/**/*.cpp']) r
    JOIN corpus ON r.file_path = corpus.path
    GROUP BY file_path
), compared AS (
    SELECT sent.path, sent.receipt.status AS status,
        sent.receipt.error AS error, sent.receipt.body AS returned_body,
        direct.body AS direct_body, direct.ast_rows
    FROM sent LEFT JOIN direct USING (path)
), summary AS (
    SELECT count(path) AS compared_files, count(DISTINCT path) AS distinct_files,
        sum(ast_rows) AS direct_ast_rows,
        count(path) FILTER (WHERE status IS DISTINCT FROM 200 OR error IS NOT NULL) AS request_failures,
        count(path) FILTER (WHERE direct_body IS NULL) AS missing_direct_files,
        count(path) FILTER (WHERE returned_body IS DISTINCT FROM direct_body) AS ast_mismatches,
        min(path) FILTER (WHERE status IS DISTINCT FROM 200 OR error IS NOT NULL
            OR direct_body IS NULL OR returned_body IS DISTINCT FROM direct_body) AS first_failed_path,
        min(status) FILTER (WHERE status IS DISTINCT FROM 200 OR error IS NOT NULL
            OR returned_body IS DISTINCT FROM direct_body) AS failed_status,
        min(error) FILTER (WHERE error IS NOT NULL) AS first_error,
        min(left(returned_body, 240)) FILTER (WHERE returned_body IS DISTINCT FROM direct_body) AS failed_body_sample
    FROM compared
)
SELECT compared_files, distinct_files, direct_ast_rows, request_failures,
    missing_direct_files, ast_mismatches, first_failed_path, failed_status,
    first_error, failed_body_sample,
    CASE WHEN compared_files = 256 AND distinct_files = 256
        AND direct_ast_rows = 541619 AND request_failures = 0
        AND missing_direct_files = 0 AND ast_mismatches = 0 THEN 'PASS'
    ELSE error(printf('AST self-dispatch mismatch: compared=%s distinct=%s direct_rows=%s requests=%s missing_direct=%s arrays=%s first_path=%s status=%s error=%s body=%s',
        compared_files::VARCHAR, distinct_files::VARCHAR,
        coalesce(direct_ast_rows, -1)::VARCHAR,
        request_failures::VARCHAR, missing_direct_files::VARCHAR,
        ast_mismatches::VARCHAR,
        coalesce(first_failed_path, '<none>'),
        coalesce(failed_status::VARCHAR, '<none>'), coalesce(first_error, '<none>'),
        coalesce(failed_body_sample, '<none>'))) END AS verdict
FROM summary;

SELECT * FROM quackapi_stop();
