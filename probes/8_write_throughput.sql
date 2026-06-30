-- ============================================================================
-- probes/8_write_throughput.sql (fixed)
-- Edge #8 probe. Run combined: cat dispatch_local.sql this.sql | duckdb :memory:
-- Server up on 9497. Kill server after to query file for final counts.
-- ============================================================================
.timer on

-- 0) Setup via list literal dispatch (simple, no CTE scope issues)
SELECT 'setup' AS phase, idx, ok, row_count, response
FROM dispatch([
  'CREATE OR REPLACE TABLE write_log (seq BIGINT, val BIGINT)',
  'CREATE OR REPLACE TABLE write_conflict (id BIGINT PRIMARY KEY, cnt BIGINT)',
  'INSERT INTO write_conflict (id, cnt) VALUES (1, 0)'
], nthreads := 1);

-- 1-3) Throughput: build list in one expression, dispatch N inserts non-conflicting
-- 256 inserts for measurable time; use different seq.
SELECT 'throughput_1' AS phase, array_length(list(ok)) AS total, array_length(list_filter(list(ok), lambda x : x)) AS oks
FROM dispatch( list_transform( range(0,256), lambda i : 'INSERT INTO write_log (seq, val) VALUES (' || i || ', ' || (i*10) || ')' ) , nthreads := 1 );

SELECT 'throughput_8' AS phase, array_length(list(ok)) AS total, array_length(list_filter(list(ok), lambda x : x)) AS oks
FROM dispatch( list_transform( range(0,256), lambda i : 'INSERT INTO write_log (seq, val) VALUES (' || i || ', ' || (i*10) || ')' ) , nthreads := 8 );

SELECT 'throughput_16' AS phase, array_length(list(ok)) AS total, array_length(list_filter(list(ok), lambda x : x)) AS oks
FROM dispatch( list_transform( range(0,256), lambda i : 'INSERT INTO write_log (seq, val) VALUES (' || i || ', ' || (i*10) || ')' ) , nthreads := 16 );

-- 4) Conflict without retry: 16 updates to same row, expect ~5-7 ok, rest Conflict
SELECT 'conflict_no_retry' AS phase, idx, ok, row_count, substr(response, 1, 90) AS head
FROM dispatch( list_transform( range(0,16), lambda i : 'UPDATE write_conflict SET cnt = cnt + 1 WHERE id=1' ) , nthreads := 16, max_retries := 0 );

-- 5) With retries
SELECT 'conflict_retry' AS phase, array_length(list(ok)) AS total, array_length(list_filter(list(ok), lambda x : x)) AS oks
FROM dispatch( list_transform( range(0,16), lambda i : 'UPDATE write_conflict SET cnt = cnt + 1 WHERE id=1' ) , nthreads := 16, max_retries := 30 );

-- Note: for final counts, kill server then query the file db directly (client lock otherwise)
