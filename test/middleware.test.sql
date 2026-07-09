-- ============================================================================
-- quackapi middleware integration tests
-- ============================================================================
-- Load order (stdin pipe to duckdb):
--   framework.sql | middleware.sql | this file
--
-- STUB: dispatch_async
--   The real primitive is the C-layer self-dispatch function from serve_brain
--   (built in a parallel agent).  Here we stub it as a TABLE macro that:
--     1. appends the SQL (hex-encoded) to quackapi_bg_log.txt via shellfs
--     2. returns a single row: status = 'ok'
--
--   INTEGRATION NOTE: when the C layer is wired in, delete everything between
--   the "BEGIN STUB" and "END STUB" markers below.  middleware.sql's
--   enqueue_background macro is unchanged — it calls dispatch_async as a TABLE
--   macro; the real primitive will expose the same interface.
-- ============================================================================
INSTALL shellfs FROM community; LOAD shellfs;

-- =================== BEGIN STUB (remove at integration) ====================

-- Reset the bg log file at test-suite start.
SELECT content FROM read_text('touch quackapi_bg_log.txt && > quackapi_bg_log.txt && echo ok |');

-- dispatch_async stub: appends hex(sql) as a line to the log file.
-- Uses touch to ensure the file exists before appending (>> to a non-existent
-- file is silently swallowed by the shellfs shell in some environments).
-- Returns one row: status = 'ok'.
CREATE OR REPLACE MACRO dispatch_async(sql) AS TABLE (
  SELECT content AS status
  FROM read_text(
    'touch quackapi_bg_log.txt && echo '
    || hex(sql)
    || ' >> quackapi_bg_log.txt && echo ok |'
  )
);

-- _bg_log: view that decodes each hex line back to a SQL string.
-- Enables asserts like: SELECT * FROM _bg_log WHERE sql_text = '...'
CREATE OR REPLACE VIEW _bg_log AS
SELECT decode(unhex(trim(line))) AS sql_text
FROM read_csv(
  'cat quackapi_bg_log.txt |',
  header  = false,
  columns = {'line': 'VARCHAR'},
  delim   = chr(1)    -- single-byte delimiter not present in hex strings
);

-- =================== END STUB (remove at integration) ======================

-- ---------------------------------------------------------------------------
-- Test helpers
-- ---------------------------------------------------------------------------
CREATE OR REPLACE MACRO assert_true(cond, label) AS (
  CASE WHEN cond THEN 'PASS: ' || label
       ELSE error('FAIL: ' || label)
  END
);

-- ---------------------------------------------------------------------------
-- TEST 1 — apply_pre: authenticated request PASSES
-- ---------------------------------------------------------------------------
SELECT assert_true(
  pass = true AND status_code = 200,
  'pre: authenticated request passes'
)
FROM apply_pre(
  'GET',
  '/users/1',
  '{"authorization":"Bearer valid-token-abc","_cookies":{}}',
  ''
);

-- ---------------------------------------------------------------------------
-- TEST 2 — apply_pre: missing Authorization header -> 401 missing_credentials
-- ---------------------------------------------------------------------------
SELECT assert_true(
  pass = false
  AND status_code = 401
  AND json_extract_string(body, '$.code') = 'missing_credentials',
  'pre: missing auth header -> 401 missing_credentials'
)
FROM apply_pre('GET', '/users/1', '{}', '');

-- ---------------------------------------------------------------------------
-- TEST 3 — apply_pre: wrong scheme (Basic instead of Bearer) -> 401 bad_scheme
-- ---------------------------------------------------------------------------
SELECT assert_true(
  pass = false
  AND status_code = 401
  AND json_extract_string(body, '$.code') = 'bad_scheme',
  'pre: wrong auth scheme -> 401 bad_scheme'
)
FROM apply_pre(
  'GET',
  '/users/1',
  '{"authorization":"Basic dXNlcjpwYXNz"}',
  ''
);

-- ---------------------------------------------------------------------------
-- TEST 4 — apply_pre: request_logger populates log_entry with method
-- ---------------------------------------------------------------------------
SELECT assert_true(
  pass = true
  AND len(log_entry) > 0
  AND json_extract_string(log_entry, '$.method') = 'GET',
  'pre: request_logger writes log_entry with method'
)
FROM apply_pre(
  'GET',
  '/users/42',
  '{"authorization":"Bearer tok","_cookies":{}}',
  ''
);

-- ---------------------------------------------------------------------------
-- TEST 5 — apply_post: header_injector adds X-Powered-By and X-Frame-Options
-- ---------------------------------------------------------------------------
SELECT assert_true(
  json_extract_string(resp_headers, '$."X-Powered-By"')    = 'quackapi/0.1'
  AND json_extract_string(resp_headers, '$."X-Frame-Options"') = 'DENY',
  'post: header_injector injects X-Powered-By and X-Frame-Options'
)
FROM apply_post(200, 'application/json', '{"ok":true}', '{}');

-- ---------------------------------------------------------------------------
-- TEST 6 — apply_post: pre-existing response headers are preserved
-- ---------------------------------------------------------------------------
SELECT assert_true(
  json_extract_string(resp_headers, '$."Content-Language"') = 'en'
  AND json_extract_string(resp_headers, '$."X-Powered-By"') = 'quackapi/0.1',
  'post: header_injector preserves pre-existing response headers'
)
FROM apply_post(200, 'application/json', '{"ok":true}', '{"Content-Language":"en"}');

-- ---------------------------------------------------------------------------
-- TEST 7 — apply_post: null resp_headers treated as empty object
-- ---------------------------------------------------------------------------
SELECT assert_true(
  json_extract_string(resp_headers, '$."X-Powered-By"') = 'quackapi/0.1',
  'post: header_injector handles null resp_headers'
)
FROM apply_post(200, 'application/json', 'hello', NULL);

-- ---------------------------------------------------------------------------
-- TEST 8 — enqueue_background: stub records the SQL in _bg_log
-- ---------------------------------------------------------------------------
-- Reset log before this test.
SELECT content FROM read_text('> quackapi_bg_log.txt && echo ok |');
SELECT status  FROM enqueue_background('SELECT 42 AS bg_task');

SELECT assert_true(
  (SELECT array_length(array_agg(sql_text)) FROM _bg_log
   WHERE sql_text = 'SELECT 42 AS bg_task') = 1,
  'enqueue_background: stub records row in _bg_log'
);

-- ---------------------------------------------------------------------------
-- TEST 9 — end-to-end: pre pass -> handle_request 200 -> post injects header
-- ---------------------------------------------------------------------------
WITH
pre AS (
  SELECT * FROM apply_pre(
    'GET', '/users',
    '{"authorization":"Bearer tok"}',
    ''
  )
),
routed AS (
  SELECT * FROM handle_request('GET', '/users', '{"authorization":"Bearer tok"}', '')
),
post AS (
  SELECT * FROM apply_post(
    (SELECT status_code FROM routed),
    (SELECT content_type FROM routed),
    (SELECT body        FROM routed),
    '{}'
  )
)
SELECT assert_true(
  (SELECT pass        FROM pre)   = true
  AND (SELECT status_code FROM routed) = 200
  AND json_extract_string((SELECT resp_headers FROM post), '$."X-Powered-By"') = 'quackapi/0.1',
  'e2e: authenticated /users -> 200 with X-Powered-By injected'
);

-- ---------------------------------------------------------------------------
-- TEST 10 — end-to-end: unauthenticated request short-circuits at pre
-- ---------------------------------------------------------------------------
WITH pre AS (
  SELECT * FROM apply_pre('GET', '/users', '{}', '')
)
SELECT assert_true(
  (SELECT pass        FROM pre) = false
  AND (SELECT status_code FROM pre) = 401,
  'e2e: unauthenticated request short-circuits at pre with 401'
);

-- ---------------------------------------------------------------------------
-- R1 CORS tests (pre flight short + post ACAO)
-- Uses the seeded cors row (priority 5, allowed example.com)
-- ---------------------------------------------------------------------------
SELECT assert_true(
  pass = false AND status_code = 200
  AND json_extract_string(resp_headers, '$.Access-Control-Allow-Origin') = 'https://example.com'
  AND json_extract_string(resp_headers, '$.Access-Control-Allow-Methods') LIKE '%POST%',
  'R1: cors preflight OPTIONS short-circuits 200 with ACA* headers'
)
FROM apply_pre(
  'OPTIONS',
  '/anything',
  '{"origin":"https://example.com","access-control-request-method":"POST"}',
  ''
);

SELECT assert_true(
  json_extract_string(resp_headers, '$.Access-Control-Allow-Origin') = 'https://example.com',
  'R1: cors post injects ACAO on normal response'
)
FROM apply_post(200, 'application/json', '{}', '{}');

-- ---------------------------------------------------------------------------
-- Summary banner
-- ---------------------------------------------------------------------------
SELECT '=== ALL MIDDLEWARE TESTS PASSED ===' AS result;
