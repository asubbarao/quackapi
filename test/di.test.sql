-- ============================================================================
-- quackapi/test/di.test.sql  —  DI module tests
-- ============================================================================
-- Run: duckdb -c "$(cat framework.sql)" < test/di.test.sql
-- Or:  duckdb < <(cat framework.sql di.sql test/di.test.sql)
-- ─────────────────────────────────────────────────────────────────────────────

-- ── 1. providers table seeded correctly ──────────────────────────────────────
SELECT '=== TEST 1: providers table ===' AS test;
SELECT id, name, scope FROM providers ORDER BY id;

-- ── 2. tokens table seeded and joined to users ────────────────────────────────
SELECT '=== TEST 2: tokens join users ===' AS test;
SELECT t.token, u.name, u.age
FROM tokens t
JOIN users u ON u.id = t.user_id
ORDER BY t.token;

-- ── 3. di_resolve — valid token resolves current_user ────────────────────────
SELECT '=== TEST 3: di_resolve valid token ===' AS test;
WITH ctx AS (
  SELECT di_resolve('{"authorization":"Bearer tok-alice-001"}') AS j
)
SELECT
  json_extract_string(j, '$.db_session')               AS db_session,
  json_extract_string(j, '$.current_user.name')        AS resolved_name,
  json_extract_string(j, '$.current_user.user_id')     AS resolved_user_id,
  len(json_extract_string(j, '$.request_id')) > 30     AS has_request_id
FROM ctx;

-- ── 4. di_resolve — invalid token → current_user null ────────────────────────
SELECT '=== TEST 4: di_resolve invalid token ===' AS test;
WITH ctx AS (
  SELECT di_resolve('{"authorization":"Bearer BOGUS"}') AS j
)
SELECT
  json_extract_string(j, '$.db_session')           AS db_session,
  json_extract(j, '$.current_user')                AS current_user_raw,
  len(json_extract_string(j, '$.request_id')) > 30 AS has_request_id
FROM ctx;

-- ── 5. di_resolve — missing authorization header → current_user null ─────────
SELECT '=== TEST 5: di_resolve missing auth ===' AS test;
WITH ctx AS (
  SELECT di_resolve('{"_cookies":{"sid":"xyz"}}') AS j
)
SELECT
  json_extract_string(j, '$.db_session')           AS db_session,
  json_extract(j, '$.current_user')                AS current_user_raw,
  len(json_extract_string(j, '$.request_id')) > 30 AS has_request_id
FROM ctx;

-- ── 6. di_resolve — second token (bob) ───────────────────────────────────────
SELECT '=== TEST 6: di_resolve tok-bob-002 ===' AS test;
WITH ctx AS (
  SELECT di_resolve('{"authorization":"Bearer tok-bob-002"}') AS j
)
SELECT
  json_extract_string(j, '$.current_user.name')    AS resolved_name,
  json_extract_string(j, '$.current_user.age')     AS resolved_age
FROM ctx;

-- ── 7. di_teardown — runs cleanly, marks session closed ──────────────────────
SELECT '=== TEST 7: di_teardown ===' AS test;
WITH
  ctx AS (
    SELECT di_resolve('{"authorization":"Bearer tok-carol-003"}') AS context_json
  ),
  td AS (
    SELECT di_teardown(context_json) AS j, context_json FROM ctx
  )
SELECT
  json_extract_string(td.j, '$.db_session')                            AS db_session_after,
  json_extract_string(td.j, '$.request_id')
    = json_extract_string(td.context_json, '$.request_id')             AS request_id_matches,
  json_extract_string(td.j, '$.teardown_at') IS NOT NULL               AS has_teardown_at
FROM td;

-- ── 8. Full per-request lifecycle simulation ──────────────────────────────────
-- Models: BEGIN → di_resolve → [handler reads context] → di_teardown → COMMIT
SELECT '=== TEST 8: full lifecycle simulation ===' AS test;
WITH
  ctx    AS (SELECT di_resolve('{"authorization":"Bearer tok-alice-001","_cookies":{"sid":"s1"}}') AS context_json),
  -- handler: read whoami from context (simulated — just extract current_user)
  result AS (
    SELECT json_extract_string(context_json, '$.current_user.name') AS handler_output
    FROM ctx
  ),
  td     AS (SELECT di_teardown((SELECT context_json FROM ctx)) AS teardown_json)
SELECT
  r.handler_output,
  json_extract_string(t.teardown_json, '$.db_session') AS final_db_session
FROM result r, td t;
