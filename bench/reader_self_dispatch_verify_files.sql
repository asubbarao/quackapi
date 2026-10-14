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
        sent.receipt.error AS error, left(sent.receipt.body, 240) AS body_sample,
        try_cast(try_cast(sent.receipt.body AS JSON)
            AS STRUCT(filename VARCHAR, content VARCHAR)[]) AS returned_rows,
        [struct_pack(filename := sent.path,
            content := CASE WHEN sent.reader = 'blob' THEN direct.content::VARCHAR
                ELSE decode(direct.content) END)] AS expected_rows,
        octet_length(direct.content) AS direct_bytes,
        direct.path IS NULL AS missing_direct
    FROM sent LEFT JOIN direct USING (path)
), summary AS (
    SELECT count(path) AS compared_files, count(DISTINCT path) AS distinct_files,
        count(path) FILTER (WHERE status IS DISTINCT FROM 200 OR error IS NOT NULL) AS request_failures,
        count(path) FILTER (WHERE missing_direct) AS missing_direct_files,
        count(path) FILTER (WHERE source_bytes IS DISTINCT FROM direct_bytes) AS changed_source_bytes,
        count(path) FILTER (WHERE returned_rows IS DISTINCT FROM expected_rows) AS row_mismatches,
        min(path) FILTER (WHERE status IS DISTINCT FROM 200 OR error IS NOT NULL
            OR missing_direct OR source_bytes IS DISTINCT FROM direct_bytes
            OR returned_rows IS DISTINCT FROM expected_rows) AS first_failed_path,
        min(status) FILTER (WHERE status IS DISTINCT FROM 200 OR error IS NOT NULL
            OR returned_rows IS DISTINCT FROM expected_rows) AS failed_status,
        min(error) FILTER (WHERE error IS NOT NULL) AS first_error,
        min(body_sample) FILTER (WHERE returned_rows IS DISTINCT FROM expected_rows) AS failed_body_sample
    FROM compared
)
SELECT compared_files, distinct_files, request_failures, missing_direct_files,
    changed_source_bytes, row_mismatches, first_failed_path, failed_status,
    first_error, failed_body_sample,
    CASE WHEN compared_files = 4097 AND distinct_files = 4097
        AND request_failures = 0 AND missing_direct_files = 0
        AND changed_source_bytes = 0 AND row_mismatches = 0 THEN 'PASS'
    ELSE error(printf('file self-dispatch mismatch: compared=%s distinct=%s requests=%s missing_direct=%s source_bytes=%s rows=%s first_path=%s status=%s error=%s body=%s',
        compared_files::VARCHAR, distinct_files::VARCHAR, request_failures::VARCHAR,
        missing_direct_files::VARCHAR, changed_source_bytes::VARCHAR,
        row_mismatches::VARCHAR, coalesce(first_failed_path, '<none>'),
        coalesce(failed_status::VARCHAR, '<none>'), coalesce(first_error, '<none>'),
        coalesce(failed_body_sample, '<none>'))) END AS verdict
FROM summary;

SELECT * FROM quackapi_stop();
