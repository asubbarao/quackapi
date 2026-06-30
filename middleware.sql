-- ============================================================================
-- quackapi middleware chain + background task enqueue
-- ============================================================================
-- Provides:
--   middleware              TABLE  — ordered config-as-data chain
--   apply_pre(...)          MACRO  — pre-phase chain; returns decision row
--   apply_post(...)         MACRO  — post-phase chain; returns transformed response
--   enqueue_background(sql) MACRO  — fire-and-forget background task
--
-- Depends on: framework.sql (routes, param_schema, handle_request)
-- Integration: dispatch_async(sql VARCHAR) is wired in by the C layer
--   (serve_brain / self-dispatch module built in parallel).
--   In test/middleware.test.sql a local TABLE-macro stub replaces it.
-- ============================================================================

-- ---------------------------------------------------------------------------
-- 1. middleware config table
-- ---------------------------------------------------------------------------
CREATE OR REPLACE TABLE middleware (
  id           VARCHAR,
  phase        VARCHAR,   -- 'pre' | 'post'
  priority     INTEGER,   -- lower number = runs first within a phase
  kind         VARCHAR,   -- 'request_logger' | 'auth_gate' | 'header_injector'
  config_json  VARCHAR    -- JSON blob of step-specific settings
);

-- Seed from JSON array — same json_each('[...]'::JSON) pattern as routes.
INSERT INTO middleware
SELECT
  json_extract_string(value, '$.id'),
  json_extract_string(value, '$.phase'),
  CAST(json_extract_string(value, '$.priority') AS INTEGER),
  json_extract_string(value, '$.kind'),
  json_extract_string(value, '$.config_json')
FROM json_each('[
  {
    "id":          "pre_logger",
    "phase":       "pre",
    "priority":    10,
    "kind":        "request_logger",
    "config_json": "{\"log_headers\": true}"
  },
  {
    "id":          "pre_auth",
    "phase":       "pre",
    "priority":    20,
    "kind":        "auth_gate",
    "config_json": "{\"scheme\": \"Bearer\", \"required\": true}"
  },
  {
    "id":          "post_headers",
    "phase":       "post",
    "priority":    10,
    "kind":        "header_injector",
    "config_json": "{\"headers\": {\"X-Powered-By\": \"quackapi/0.1\", \"X-Frame-Options\": \"DENY\"}}"
  }
]'::JSON);

-- ---------------------------------------------------------------------------
-- 2. apply_pre(method, path, headers, body) -> TABLE
--
-- Runs every 'pre' middleware step in priority order.
-- Returns one row:
--   pass         BOOLEAN  — true  = proceed to route handler
--                           false = short-circuit with the error fields below
--   status_code  INTEGER
--   content_type VARCHAR
--   body         VARCHAR  — JSON error body (only meaningful when pass=false)
--   log_entry    VARCHAR  — JSON summary written by the request_logger step
--
-- DuckDB v1.5.3 lambda constraints:
--   - list_reduce 3-arg (with initial value) is NOT supported.
--   - Lambdas cannot contain correlated subqueries into outer CTEs.
--   Strategy: pre-compute each step's outcome as a table row, then pick the
--   first failure (if any) and the logger's log entry with plain SQL.
--
-- Auth gate rule (no LIKE / ILIKE / regexp):
--   starts_with(authorization_header, scheme || ' ')
-- ---------------------------------------------------------------------------
CREATE OR REPLACE MACRO apply_pre(method, path, headers, body) AS TABLE (
WITH

-- Pre-extract shared scalars so step logic references plain column values.
ctx AS (
  SELECT
    json_extract_string(headers, '$.authorization') AS auth_val,
    cast(json_object('method', method, 'path', path, 'headers', headers) AS VARCHAR) AS log_json
),

-- Evaluate each pre-phase step: does it fail? If so, what response?
step_outcomes AS (
  SELECT
    m.priority,
    m.kind,

    CASE m.kind
      WHEN 'request_logger' THEN false
      WHEN 'auth_gate' THEN
        (SELECT auth_val FROM ctx) IS NULL
        OR NOT starts_with(
             (SELECT auth_val FROM ctx),
             json_extract_string(m.config_json, '$.scheme') || ' '
           )
      ELSE false
    END AS step_fails,

    CASE m.kind
      WHEN 'auth_gate' THEN 401
      ELSE 200
    END AS fail_status,

    CASE m.kind
      WHEN 'auth_gate' THEN
        CASE
          WHEN (SELECT auth_val FROM ctx) IS NULL
            THEN cast(json_object(
                   'detail', 'Not authenticated',
                   'code',   'missing_credentials'
                 ) AS VARCHAR)
          ELSE cast(json_object(
                 'detail', 'Invalid authentication scheme',
                 'code',   'bad_scheme'
               ) AS VARCHAR)
        END
      ELSE NULL
    END AS fail_body,

    CASE m.kind
      WHEN 'request_logger' THEN (SELECT log_json FROM ctx)
      ELSE NULL
    END AS log_entry

  FROM middleware m
  WHERE m.phase = 'pre'
),

-- First failing step (NULL when all pass).
first_fail AS (
  SELECT fail_status, fail_body
  FROM step_outcomes
  WHERE step_fails
  ORDER BY priority
  LIMIT 1
),

-- Log entry from the request_logger step.
logger AS (
  SELECT log_entry
  FROM step_outcomes
  WHERE kind = 'request_logger'
  LIMIT 1
)

SELECT
  (SELECT fail_status FROM first_fail) IS NULL           AS pass,
  COALESCE((SELECT fail_status FROM first_fail), 200)    AS status_code,
  'application/json'::VARCHAR                            AS content_type,
  COALESCE((SELECT fail_body   FROM first_fail), '')     AS body,
  COALESCE((SELECT log_entry   FROM logger),    '')      AS log_entry
);

-- ---------------------------------------------------------------------------
-- 3. apply_post(status, content_type, body, resp_headers) -> TABLE
--
-- Runs every 'post' middleware step in priority order over the outgoing
-- response.  Returns one row:
--   status_code  INTEGER
--   content_type VARCHAR
--   body         VARCHAR
--   resp_headers VARCHAR  — JSON object of response headers (augmented)
--
-- header_injector: merges config-driven headers into resp_headers using
--   json_merge_patch.  Uses 2-arg list_reduce (DuckDB 1.5.3 doesn't support
--   3-arg form).  The base headers string is prepended to the patches list
--   so the first element acts as the initial accumulator.
-- ---------------------------------------------------------------------------
CREATE OR REPLACE MACRO apply_post(status, content_type, body, resp_headers) AS TABLE (
WITH

-- Normalise null/empty resp_headers to an empty JSON object.
base AS (
  SELECT CASE
    WHEN resp_headers IS NULL OR len(resp_headers) = 0 THEN '{}'
    ELSE resp_headers
  END AS rh
),

-- Ordered list of header patches from header_injector steps.
-- Prepend the base headers string so 2-arg list_reduce uses it as seed.
patches AS (
  SELECT
    list_prepend(
      (SELECT rh FROM base),
      list(cast(json_extract(m.config_json, '$.headers') AS VARCHAR) ORDER BY m.priority)
    ) AS patch_list
  FROM middleware m
  WHERE m.phase = 'post' AND m.kind = 'header_injector'
),

-- Fold: merge each header patch in order.
merged AS (
  SELECT
    CASE
      WHEN (SELECT array_length(patch_list) FROM patches) <= 1
           OR (SELECT patch_list FROM patches) IS NULL
        THEN (SELECT rh FROM base)
      ELSE
        list_reduce(
          (SELECT patch_list FROM patches),
          lambda acc, patch: cast(json_merge_patch(acc::JSON, patch::JSON) AS VARCHAR)
        )
    END AS final_headers
)

SELECT
  status        AS status_code,
  content_type,
  body,
  (SELECT final_headers FROM merged) AS resp_headers
);

-- ---------------------------------------------------------------------------
-- 4. dispatch_async placeholder + enqueue_background
--
-- dispatch_async(sql VARCHAR) -> TABLE(status VARCHAR)
--   INTERFACE CONTRACT: this signature is the agreed boundary between
--   middleware.sql and the C-layer self-dispatch module (built in parallel).
--   The placeholder below raises a clear error at call time so any accidental
--   call without wiring is immediately obvious.  The test stub in
--   test/middleware.test.sql overrides this with CREATE OR REPLACE.
--   At integration the C layer replaces it with the real implementation.
--
-- enqueue_background(sql) -> TABLE
--   Fire-and-forget: calls dispatch_async and discards the result row.
--   Callers use:  SELECT status FROM enqueue_background('SELECT ...')
-- ---------------------------------------------------------------------------
CREATE OR REPLACE MACRO dispatch_async(sql) AS TABLE (
  SELECT error(
    'dispatch_async not wired: load the C-layer self-dispatch module '
    || 'or the test stub before calling enqueue_background'
  ) AS status
  WHERE false
);

CREATE OR REPLACE MACRO enqueue_background(sql) AS TABLE (
  SELECT status FROM dispatch_async(sql)
);
