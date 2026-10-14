-- Exact 4,097-file body comparison: same-instance POST response content versus
-- DuckDB's direct read_blob over the pinned source tree. Text bytes are decoded
-- as UTF-8; BLOBs use DuckDB's exact BLOB-to-VARCHAR escape representation.
SET threads = 4;
SET memory_limit = '4GB';
SET preserve_insertion_order = false;
LOAD hostfs;
LOAD json;

CREATE ROUTE reader_dispatch POST '/dispatch' AS SELECT * FROM query($sql);
SELECT * FROM quackapi_serve(getenv('READER_BENCH_PORT')::INTEGER,
    host := '127.0.0.1', worker_threads := 64, access_log := false,
    compression := false, query_timeout_ms := 120000,
    max_response_bytes := 16777216, block := false);
SET curl_httpfs_client_implementation = 'multi_curl';
SELECT curl_httpfs_http_util_name() AS provider;

WITH candidates AS (
    SELECT file FROM glob(getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/src/**/*')
    UNION ALL SELECT file FROM glob(getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/test/**/*')
    UNION ALL SELECT file FROM glob(getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/tools/**/*')
    UNION ALL SELECT file FROM glob(getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/extension/**/*')
), eligible AS (
    SELECT file AS path, file_size(file) AS source_bytes FROM candidates
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
    SELECT row_number() OVER (ORDER BY path) AS ordinal, path, source_bytes FROM corpus_paths
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
), direct AS (
    SELECT filename AS path, content FROM read_blob([
        getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/src/**/*',
        getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/test/**/*',
        getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/tools/**/*',
        getenv('READER_BENCH_SOURCE_ROOT') || '/duckdb/extension/**/*'])
), compared AS (
    SELECT sent.path, sent.reader, sent.source_bytes, sent.receipt.status AS status,
        sent.receipt.error AS error,
        json_extract_string(sent.receipt.body::JSON, '$[0].filename') AS returned_path,
        json_extract_string(sent.receipt.body::JSON, '$[0].content') AS returned_content,
        CASE WHEN sent.reader = 'blob' THEN direct.content::VARCHAR
            ELSE decode(direct.content) END AS direct_content,
        octet_length(direct.content) AS direct_bytes,
        json_array_length(sent.receipt.body::JSON) AS returned_rows
    FROM sent JOIN direct USING (path)
)
SELECT count(path) AS compared_files,
    count(path) FILTER (WHERE status <> 200 OR error IS NOT NULL) AS request_failures,
    count(path) FILTER (WHERE returned_rows <> 1) AS wrong_row_counts,
    count(path) FILTER (WHERE path <> returned_path) AS wrong_paths,
    count(path) FILTER (WHERE source_bytes <> direct_bytes) AS changed_source_bytes,
    count(path) FILTER (WHERE returned_content IS DISTINCT FROM direct_content) AS content_mismatches,
    min(path) FILTER (WHERE returned_content IS DISTINCT FROM direct_content) AS first_mismatch
FROM compared;

SELECT * FROM quackapi_stop();
