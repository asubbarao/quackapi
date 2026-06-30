-- ============================================================================
-- probes/6_di_setup_teardown.sql
-- Edge #6: Dependency injection w/ setup+teardown.
-- Hypothesis: PARTIAL -- setup/inject via data works; true finally/teardown on error path
-- and resource lifetime not guaranteed like yield/finally in FastAPI.
-- Probe: model providers as data + sequential dispatch for setup/handler/teardown;
--        force handler error, observe if teardown step "guaranteed".
-- ============================================================================
.timer on

-- model: side effect log table for setup/teardown runs
SELECT 'model' AS p, idx, ok FROM dispatch([
  'CREATE OR REPLACE TABLE di_log (step VARCHAR, ts TIMESTAMP)',
  'CREATE OR REPLACE TABLE di_work (id BIGINT)'
], nthreads := 1);

-- "resolve" setup: like di_resolve, run setup "sql" as dispatch before "handler"
SELECT 'setup' AS p, idx, ok, response FROM dispatch([
  'INSERT INTO di_log (step, ts) VALUES (''setup'', now())',
  'INSERT INTO di_work (id) VALUES (42)'
], nthreads := 1);

-- "handler" success path
SELECT 'handler_ok' AS p, idx, ok, response FROM dispatch([
  'SELECT id FROM di_work WHERE id=42'
], nthreads := 1);

-- teardown after
SELECT 'teardown_ok' AS p, idx, ok, response FROM dispatch([
  'INSERT INTO di_log (step, ts) VALUES (''teardown_ok'', now())'
], nthreads := 1);

-- now simulate error mid-flight: bad handler
SELECT 'handler_error' AS p, idx, ok, response FROM dispatch([
  'INSERT INTO di_work (id) VALUES (''not_int'')'
], nthreads := 1);

-- after error, do we still teardown? (in our script yes, but no automatic guarantee if dispatch caller aborts on error)
SELECT 'teardown_after_err' AS p, idx, ok, response FROM dispatch([
  'INSERT INTO di_log (step, ts) VALUES (''teardown_after_err_attempt'', now())'
], nthreads := 1);

-- observe log (final state after kill server)
SELECT 'log_state' AS p, response FROM dispatch([
  'SELECT array_agg(step ORDER BY step) AS steps FROM di_log'
], nthreads := 1);