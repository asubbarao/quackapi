-- ============================================================================
-- test/retry_probe.test.sql : demonstrate httpfs_timeout_retry behavior
-- ============================================================================
-- Run standalone (stdin-piped — the probe contains INTENTIONAL failing
-- statements to force timeouts/retries; stdin keeps going past a failed stmt so
-- the retries=0 vs retries=3 contrast is visible):
--   duckdb -unsigned < test/retry_probe.test.sql
-- Or with the framework (which also loads+configures the ext):
--   cat framework.sql test/retry_probe.test.sql | duckdb -unsigned
--
-- Observes transport retry via:
--   - .timer on (per-statement times)
--   - wall-clock via repeated runs or delay in output
--   - different total latency for retries=0 vs retries>0 against slow/timeout endpoints
-- Uses only public httpbin.org (no servers started, no forbidden ports).
--
-- What it retries (from extension):
--   - file ops (open/read/write for read_csv/read_json/read_parquet etc): httpfs_*_file_operation
--   - list, stat, delete, create_dir similarly
-- Defaults/knobs:
--   - per-op *_ms (millis) and *_retries take precedence
--   - fall back to http_timeout (seconds), http_retries, http_retry_wait_ms, http_retry_backoff
--   - when unset (NULL after load), base http_ values apply
-- Extension adds NO functions; purely settings + wrapped httpfs transport.

INSTALL httpfs_timeout_retry FROM community;
LOAD httpfs_timeout_retry;

-- Ensure curl client (as soldered in framework.sql)
SET httpfs_client_implementation = 'curl';

-- Show the knobs the extension registers
SELECT '=== 1. httpfs_timeout_retry settings (added by extension) ===' AS probe;
SELECT name, value
FROM duckdb_settings()
WHERE name LIKE 'httpfs_%timeout%' OR name LIKE 'httpfs_%retries%'
ORDER BY name;

SELECT '=== 2. base http retry knobs (fallbacks) ===' AS probe;
SELECT name, value
FROM duckdb_settings()
WHERE name IN ('http_timeout', 'http_retries', 'http_retry_wait_ms', 'http_retry_backoff')
ORDER BY name;

.timer on

-- Probe A: fast path (no timeout, no retry)
SELECT '=== 3A. fast success (no retry expected) ===' AS probe;
SELECT url FROM read_json_auto('https://httpbin.org/delay/0') LIMIT 1;

-- Probe B: force timeout with 0 retries (baseline latency ~ configured timeout)
SELECT '=== 3B. timeout, retries=0 (expect ~600ms + overhead) ===' AS probe;
SET httpfs_timeout_file_operation_ms = 600;
SET httpfs_retries_file_operation = 0;
SET http_retry_wait_ms = 10;
SELECT try(url) AS url_or_null FROM read_json_auto('https://httpbin.org/delay/2') LIMIT 1;

-- Probe C: same slow endpoint, 3 retries (expect ~4 attempts * timeout-ish = multi-second)
SELECT '=== 3C. timeout + retries=3 (expect ~2s+ total; retries visible in time) ===' AS probe;
SET httpfs_timeout_file_operation_ms = 600;
SET httpfs_retries_file_operation = 3;
SET http_retry_wait_ms = 150;
SELECT try(url) AS url_or_null FROM read_json_auto('https://httpbin.org/delay/2') LIMIT 1;

-- Probe D: 5xx response triggers retries (HEAD/GET 500 path)
SELECT '=== 3D. 500 response, retries=2 (exhausts then fails; time shows attempts) ===' AS probe;
SET httpfs_timeout_file_operation_ms = 2000;
SET httpfs_retries_file_operation = 2;
SET http_retry_wait_ms = 50;
SELECT try( (SELECT url FROM read_json_auto('https://httpbin.org/status/500') LIMIT 1) ) AS url_or_null;

-- Final: current effective settings after probes
SELECT '=== 4. effective knobs at end of probe ===' AS probe;
SELECT name, value FROM duckdb_settings()
WHERE (name LIKE 'httpfs_%' OR name LIKE 'http_%retry%' OR name = 'http_timeout')
  AND (name LIKE '%timeout%' OR name LIKE '%retry%' OR name LIKE '%retries%')
ORDER BY name;

SELECT '=== retry_probe complete (transport layer only) ===' AS probe;
