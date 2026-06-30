-- ============================================================================
-- quackapi/di.sql  —  Request-scoped Dependency Injection
-- ============================================================================
--
-- FastAPI's `Depends` / yield-session equivalent in pure DuckDB SQL.
--
-- LOAD ORDER: after framework.sql (depends on the `users` table it seeds).
--
-- PUBLIC SURFACE
--   providers          TABLE   — registered dependencies (config-as-data)
--   tokens             TABLE   — Bearer-token → user_id lookup
--   di_resolve(headers)        — scalar macro → context_json VARCHAR
--   di_teardown(context_json)  — scalar macro → teardown_record VARCHAR
--
-- ─────────────────────────────────────────────────────────────────────────────
-- EDGE-LEDGER NOTE (edge #6 — "Dependency injection w/ setup+teardown")
-- ─────────────────────────────────────────────────────────────────────────────
-- FastAPI's `Depends(get_db)` with a `yield` does three things per request:
--
--   [A] SETUP    — execute code before the handler (open a DB connection / txn)
--   [B] INJECT   — pass the produced value into the handler as a parameter
--   [C] TEARDOWN — execute teardown after the response is sent, even on error
--                  (close the connection / rollback on exception, commit on success)
--
-- What this module models — FULL vs PARTIAL vs IMPOSSIBLE:
--
--   [A] SETUP — FULL for pure-computation providers (request_id, db_session marker,
--       current_user resolution). di_resolve() runs all setup logic and returns a
--       single JSON context object the handler can read. The C layer (serve_brain)
--       calls di_resolve() before invoking the handler SQL.
--
--   [B] INJECT — FULL for JSON-extractable values. The handler SQL reads
--       json_extract_string(context_json, '$.current_user.user_id') etc.
--       The injection channel is the context_json string, not typed function args,
--       so the type system cannot enforce presence at compile time — that is a
--       PARTIAL: shape is runtime-checked, not compile-time guaranteed.
--
--   [C] TEARDOWN — PARTIAL in two ways:
--       (1) "After the response is sent": DuckDB is stateless one-shot. The C layer
--           sends the response and then calls di_teardown() in a second statement.
--           This models the ordering correctly, but it is not atomic with the handler
--           — a crash between send and teardown skips teardown. FastAPI's generator
--           protocol guarantees teardown even if send() raises; we cannot replicate
--           that guarantee without wrapping the whole cycle in C.
--       (2) Real resource release (closing a DB handle, releasing a connection from
--           a pool): IMPOSSIBLE in stateless SQL. A real connection object cannot
--           live between two DuckDB statements in the same "session" because each
--           C-loop handler creates a fresh DuckDB connection (see serve_brain.sql).
--           di_teardown() records that teardown ran and can execute compensating SQL
--           (e.g., UPDATE a session-log table), but it cannot close a file handle or
--           return a pooled object — those objects do not survive a statement boundary.
--
--   TRANSACTION MODEL — HONEST ACCOUNTING:
--       FastAPI + SQLAlchemy's `get_db()` opens a txn, yields the session to the
--       handler, then commits on success or rolls back on exception. The DuckDB
--       equivalent: the C `handle_conn` thread opens a DuckDB connection, issues
--       BEGIN, calls di_resolve(), calls the handler SQL, calls di_teardown(), then
--       calls COMMIT (or ROLLBACK if any step errored). This is the correct skeleton
--       and is what the C layer should do. However, because each call is a separate
--       `duckdb_query()` invocation, an uncaught SIGSEGV in C between handler and
--       ROLLBACK would leave the txn open until the connection is garbage-collected.
--       A Python generator's `finally:` clause is more robust here — that is a REAL
--       gap that cannot be bridged in SQL alone.
--
--   SCOPED LIFETIME — HONEST ACCOUNTING:
--       FastAPI supports singleton (app), per-request, and per-endpoint scopes.
--       We support only per-request scope (the only scope a stateless one-shot can
--       model). App-scoped singletons would need a separate long-lived DuckDB
--       connection or a shared table acting as a cache — doable but not in scope here.
--
--   VERDICT: edge #6 is PARTIAL. Setup + inject = FULL for pure-SQL dependencies.
--   Teardown = modeled correctly in ordering, but resource-release semantics are
--   impossible to replicate exactly because DuckDB statements do not share object
--   lifetimes. The model is correct enough for a stateless API server; the gap is
--   honest and bounded.
-- ─────────────────────────────────────────────────────────────────────────────

-- ─── providers ───────────────────────────────────────────────────────────────
-- Registered dependencies (config-as-data). setup_sql / teardown_sql columns
-- carry the SQL snippet that each provider runs on setup / teardown.
-- NULL setup_sql means the provider is resolved by di_resolve directly (e.g.
-- current_user, which requires the headers argument, not a standalone query).
-- ─────────────────────────────────────────────────────────────────────────────
CREATE OR REPLACE TABLE providers AS
SELECT
  json_extract_string(value, '$.id')           AS id,
  json_extract_string(value, '$.name')         AS name,
  json_extract_string(value, '$.setup_sql')    AS setup_sql,
  json_extract_string(value, '$.teardown_sql') AS teardown_sql,
  json_extract_string(value, '$.scope')        AS scope
FROM json_each('[
  {
    "id":           "request_id",
    "name":         "Request ID",
    "setup_sql":    "SELECT gen_random_uuid()::VARCHAR AS request_id",
    "teardown_sql": null,
    "scope":        "request"
  },
  {
    "id":           "db_session",
    "name":         "DB Session Marker",
    "setup_sql":    "SELECT ''open'' AS db_session",
    "teardown_sql": "SELECT ''closed'' AS db_session",
    "scope":        "request"
  },
  {
    "id":           "current_user",
    "name":         "Current User (resolved from Authorization header)",
    "setup_sql":    null,
    "teardown_sql": null,
    "scope":        "request"
  }
]'::JSON);

-- ─── tokens ──────────────────────────────────────────────────────────────────
-- Bearer-token → user_id lookup table. Seeded from inline JSON (no VALUES).
-- In production, replace the json_each seed with a read_json('tokens.json')
-- or a JOIN against a persisted secrets table / external secret store.
-- Tokens map to the `users` table seeded by framework.sql (ids 1, 2, 3).
-- ─────────────────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS tokens AS
SELECT
  json_extract_string(value, '$.token')         AS token,
  json_extract_string(value, '$.user_id')::INTEGER AS user_id
FROM json_each('[
  {"token": "tok-alice-001", "user_id": 1},
  {"token": "tok-bob-002",   "user_id": 2},
  {"token": "tok-carol-003", "user_id": 3}
]'::JSON);

-- ─── di_resolve(headers) ─────────────────────────────────────────────────────
-- Runs all per-request provider setups and returns a context JSON string.
--
-- headers: JSON object string from the C layer (lower-cased keys), e.g.:
--   '{"authorization":"Bearer tok-alice-001","_cookies":{"sid":"abc"}}'
--
-- Returns VARCHAR context_json, e.g.:
--   {
--     "request_id":   "c89cd542-9cce-...",
--     "db_session":   "open",
--     "current_user": {"user_id": 1, "name": "alice", "age": 30}
--                   -- null when token absent or unrecognised
--   }
--
-- Per-request transaction model (C layer responsibility):
--   BEGIN;
--   SET context_json = di_resolve(headers);    -- provider setup
--   [execute handler SQL with context_json in scope]
--   SET teardown_rec = di_teardown(context_json);  -- provider teardown
--   COMMIT;  -- (or ROLLBACK on any error in the above)
-- ─────────────────────────────────────────────────────────────────────────────
CREATE OR REPLACE MACRO di_resolve(headers) AS (
  WITH
    -- ── provider: request_id ─────────────────────────────────────────────
    -- Generated fresh per invocation; unique across every request in the
    -- process lifetime (UUIDv4 from DuckDB's CSPRNG).
    request_id_cte AS (
      SELECT gen_random_uuid()::VARCHAR AS request_id
    ),

    -- ── provider: current_user ───────────────────────────────────────────
    -- Extract the raw token from "Authorization: Bearer <token>".
    -- string_split on ' ' gives ['Bearer', '<token>']; element 2 is the token.
    -- list_element returns NULL when the split has fewer than 2 parts (no space,
    -- missing header, etc.) — propagates gracefully as a NULL join key below.
    raw_token_cte AS (
      SELECT
        list_element(
          string_split(
            json_extract_string(headers, '$.authorization'),
            ' '
          ),
          2
        ) AS raw_token
    ),

    -- JOIN tokens → users. Left-join semantics: missing/bad token → NULL user.
    resolved_user_cte AS (
      SELECT
        u.id    AS user_id,
        u.name  AS name,
        u.age   AS age
      FROM raw_token_cte rt
      JOIN tokens  t ON t.token   = rt.raw_token
      JOIN users   u ON u.id      = t.user_id
    ),

    -- ── assemble context ─────────────────────────────────────────────────
    ctx AS (
      SELECT
        (SELECT request_id FROM request_id_cte)     AS request_id,
        'open'                                       AS db_session,
        -- NULL when token absent or unrecognised (matches FastAPI 401 pattern:
        -- callers inspect current_user and raise HTTPException themselves)
        (SELECT json_object('user_id', ru.user_id, 'name', ru.name, 'age', ru.age)
         FROM resolved_user_cte ru
         LIMIT 1)                                   AS current_user
    )
  SELECT json_object(
    'request_id',   ctx.request_id,
    'db_session',   ctx.db_session,
    'current_user', ctx.current_user
  )::VARCHAR
  FROM ctx
);

-- ─── di_teardown(context_json) ───────────────────────────────────────────────
-- Runs provider teardowns after the handler has returned and the response has
-- been sent (C layer calls this as the final step before closing the connection).
--
-- context_json: the VARCHAR returned by di_resolve() for this request.
--
-- Returns a teardown record JSON (loggable; C layer may write it to a log table):
--   {
--     "db_session":   "closed",
--     "request_id":   "<same uuid as in context>",
--     "teardown_at":  "2026-06-29T17:00:00.000Z"
--   }
--
-- Honest note: teardown_sql from the providers table is advisory metadata for
-- documentation and potential future code-generation. di_teardown() runs the
-- canonical teardown logic inline because DuckDB macros cannot iterate over a
-- table and execute arbitrary SQL snippets dynamically (no EXECUTE / eval).
-- That would require a procedural loop unavailable in pure SQL.
-- ─────────────────────────────────────────────────────────────────────────────
CREATE OR REPLACE MACRO di_teardown(context_json) AS (
  SELECT json_object(
    'db_session',   'closed',
    'request_id',   json_extract_string(context_json, '$.request_id'),
    'teardown_at',  strftime(now()::TIMESTAMPTZ, '%Y-%m-%dT%H:%M:%S.%f+00:00')
  )::VARCHAR
);
