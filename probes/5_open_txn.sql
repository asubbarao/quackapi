-- ============================================================================
-- probes/5_open_txn.sql
-- Edge #5: Open transaction across a request (yield-style DI session).
-- Hypothesis: REAL (dispatch is stateless one-shot; each POST = own conn/txn).
-- Probe: dispatch writes + reads; attempt cross-dispatch uncommitted visibility;
--        single-dispatch multi-stmt for comparison.
-- Run combined with dispatch_local from :memory: after server started.
-- No banned ops.
-- ============================================================================
.timer on

-- setup
SELECT 'setup' AS p, idx, ok FROM dispatch([
  'CREATE OR REPLACE TABLE edge5 (k BIGINT PRIMARY KEY, v BIGINT)',
  'INSERT INTO edge5 (k, v) VALUES (1, 10)'
], nthreads := 1);

-- 1) committed write then read-your-writes across separate dispatches (works, committed)
SELECT 'cross_dispatch_committed_write' AS p, idx, ok, response FROM dispatch([
  'INSERT INTO edge5 (k, v) VALUES (10, 100)',
  'SELECT v FROM edge5 WHERE k=10'
], nthreads := 1);

-- 2) "open txn" attempt: begin+insert in one dispatch, select in next -- should NOT see uncommitted
SELECT 'attempt_open_txn_write' AS p, idx, ok, response FROM dispatch([
  'BEGIN TRANSACTION',
  'INSERT INTO edge5 (k, v) VALUES (99, 999)'
], nthreads := 1);

SELECT 'read_after_open_attempt' AS p, idx, ok, response FROM dispatch([
  'SELECT v FROM edge5 WHERE k=99'
], nthreads := 1);

-- 3) single dispatch multi-stmt string: does harbor execute script with txn hold inside one POST?
-- Note: may return NDJSON or last only; observe if the insert is visible to a following select in SAME string.
SELECT 'multi_stmt_one_dispatch' AS p, idx, ok, response FROM dispatch([
  'BEGIN TRANSACTION; INSERT INTO edge5 (k, v) VALUES (77, 777); SELECT v AS inside FROM edge5 WHERE k=77; COMMIT;'
], nthreads := 1);

-- 4) after, see if 77 persisted (if the multi committed)
SELECT 'after_multi' AS p, idx, ok, response FROM dispatch([
  'SELECT v FROM edge5 WHERE k=77'
], nthreads := 1);

-- summary visibility
SELECT 'final_visible' AS p, response FROM dispatch([
  'SELECT array_agg(k ORDER BY k) AS keys FROM edge5'
], nthreads := 1);