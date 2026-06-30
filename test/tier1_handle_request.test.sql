-- ============================================================================
-- Tier-1: handle_request() direct assertions
-- Run via:  printf '.read framework.sql\n.read test/tier1_handle_request.test.sql\n' | duckdb
-- ============================================================================
-- Each check: a CTE that calls handle_request(), evaluates a boolean,
-- then a final UNION ALL of all checks → (check_name, pass).
-- A failing check is visible as pass=false.
-- A final summary row counts failures so a single scan shows overall health.
-- ============================================================================

CREATE OR REPLACE TABLE _test_results (check_name VARCHAR, pass BOOLEAN, detail VARCHAR);

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 1  GET /users/123  → 200 dynamic (body NULL, handler_sql rendered)
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/users/123','{}',''))
INSERT INTO _test_results
  SELECT 'GET /users/123 → status 200',       r.status_code = 200,           'got ' || r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'GET /users/123 → body NULL',         r.body IS NULL,                'body=' || coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'GET /users/123 → content_type json', r.content_type = 'application/json', r.content_type FROM r
  UNION ALL
  SELECT 'GET /users/123 → handler_sql not null', r.handler_sql IS NOT NULL,  coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  SELECT 'GET /users/123 → handler_sql contains id=123',
         starts_with(r.handler_sql, 'SELECT') AND
           array_length(list_filter(string_split(r.handler_sql,' '), lambda t: t = '123')) > 0,
         coalesce(r.handler_sql,'<null>') FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 2  GET /users/abc  → 422 int_parsing
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/users/abc','{}','')),
     detail_arr AS (
       SELECT json_extract_string(r.body, '$.detail') AS da FROM r
     ),
     parsed AS (
       SELECT
         r.status_code,
         r.content_type,
         r.body,
         r.handler_sql,
         -- pull first error type from detail array
         json_extract_string(json_extract(r.body,'$.detail[0]'), '$.type') AS err_type,
         json_extract_string(json_extract(r.body,'$.detail[0]'), '$.loc[1]') AS err_loc
       FROM r
     )
INSERT INTO _test_results
  SELECT 'GET /users/abc → status 422',        p.status_code = 422,           'got ' || p.status_code::VARCHAR FROM parsed p
  UNION ALL
  SELECT 'GET /users/abc → body not null',      p.body IS NOT NULL,            'body is null' FROM parsed p
  UNION ALL
  SELECT 'GET /users/abc → handler_sql null',   p.handler_sql IS NULL,         coalesce(p.handler_sql,'<null>') FROM parsed p
  UNION ALL
  SELECT 'GET /users/abc → err_type int_parsing', p.err_type = 'int_parsing',  coalesce(p.err_type,'<null>') FROM parsed p
  UNION ALL
  SELECT 'GET /users/abc → err_loc is id',      p.err_loc = 'id',              coalesce(p.err_loc,'<null>') FROM parsed p;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 3  GET /nope  → 404
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/nope','{}',''))
INSERT INTO _test_results
  SELECT 'GET /nope → status 404',         r.status_code = 404,             'got ' || r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'GET /nope → detail Not Found',
         json_extract_string(r.body, '$.detail') = 'Not Found',             coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'GET /nope → handler_sql null',   r.handler_sql IS NULL,           coalesce(r.handler_sql,'<null>') FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 4  GET /search?q=hi&limit=999  → 422 less_than_equal
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/search?q=hi&limit=999','{}','')),
     parsed AS (
       SELECT
         r.status_code,
         r.body,
         r.handler_sql,
         json_extract_string(json_extract(r.body,'$.detail[0]'), '$.type') AS err_type,
         json_extract_string(json_extract(r.body,'$.detail[0]'), '$.loc[1]') AS err_field
       FROM r
     )
INSERT INTO _test_results
  SELECT 'GET /search?limit=999 → status 422',          p.status_code = 422,            'got ' || p.status_code::VARCHAR FROM parsed p
  UNION ALL
  SELECT 'GET /search?limit=999 → err less_than_equal',  p.err_type = 'less_than_equal', coalesce(p.err_type,'<null>') FROM parsed p
  UNION ALL
  SELECT 'GET /search?limit=999 → err_field is limit',   p.err_field = 'limit',          coalesce(p.err_field,'<null>') FROM parsed p
  UNION ALL
  SELECT 'GET /search?limit=999 → handler_sql null',     p.handler_sql IS NULL,          coalesce(p.handler_sql,'<null>') FROM parsed p;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 5  GET /search?q=hi&limit=5  → 200 dynamic (valid)
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/search?q=hi&limit=5','{}',''))
INSERT INTO _test_results
  SELECT 'GET /search valid → status 200',         r.status_code = 200,          'got ' || r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'GET /search valid → body NULL',           r.body IS NULL,               coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'GET /search valid → handler_sql not null', r.handler_sql IS NOT NULL,   coalesce(r.handler_sql,'<null>') FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 6  GET /users  → 200 dynamic list_users
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/users','{}',''))
INSERT INTO _test_results
  SELECT 'GET /users → status 200',           r.status_code = 200,            'got ' || r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'GET /users → body NULL',             r.body IS NULL,                 coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'GET /users → handler_sql not null',  r.handler_sql IS NOT NULL,      coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  SELECT 'GET /users → handler_sql is SELECT', starts_with(r.handler_sql,'SELECT'), coalesce(r.handler_sql,'<null>') FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 7  POST /users valid body  → 200 dynamic create_user
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"zara","age":28}'))
INSERT INTO _test_results
  SELECT 'POST /users valid → status 200',          r.status_code = 200,           'got ' || r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'POST /users valid → body NULL',            r.body IS NULL,                coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'POST /users valid → handler_sql not null', r.handler_sql IS NOT NULL,     coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  SELECT 'POST /users valid → handler_sql has INSERT', starts_with(r.handler_sql,'INSERT'), coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  -- The name literal 'zara' must appear in the rendered SQL
  SELECT 'POST /users valid → name interpolated',
         array_length(list_filter(string_split(r.handler_sql, ''''), lambda t: t = 'zara')) > 0,
         coalesce(r.handler_sql,'<null>') FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 8  POST /users missing age  → 422 missing
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"zara"}')),
     parsed AS (
       SELECT
         r.status_code,
         r.body,
         r.handler_sql,
         -- find the error entry whose loc[1] = 'age'
         json_extract_string(
           list_filter(
             json_extract(r.body, '$.detail')::JSON[],
             lambda e: json_extract_string(e, '$.loc[1]') = 'age'
           )[1],
           '$.type'
         ) AS age_err_type
       FROM r
     )
INSERT INTO _test_results
  SELECT 'POST /users missing age → status 422',   p.status_code = 422,        'got ' || p.status_code::VARCHAR FROM parsed p
  UNION ALL
  SELECT 'POST /users missing age → handler null',  p.handler_sql IS NULL,      coalesce(p.handler_sql,'<null>') FROM parsed p
  UNION ALL
  SELECT 'POST /users missing age → err type missing', p.age_err_type = 'missing', coalesce(p.age_err_type,'<null>') FROM parsed p;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 9  POST /users missing both fields  → 422 missing (2 errors)
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','')),
     detail AS (
       SELECT json_extract(r.body, '$.detail')::JSON[] AS arr FROM r
     )
INSERT INTO _test_results
  SELECT 'POST /users empty body → status 422',    r.status_code = 422,  'got ' || r.status_code::VARCHAR FROM handle_request('POST','/users','{}','') r
  UNION ALL
  SELECT 'POST /users empty body → 2 errors',
         array_length((SELECT arr FROM detail)) = 2,
         (SELECT arr FROM detail)::VARCHAR FROM detail;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 10  GET /users/{id}/posts/{post_id}  → 200 dynamic, both params rendered
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/users/7/posts/99','{}',''))
INSERT INTO _test_results
  SELECT 'GET /users/7/posts/99 → status 200',          r.status_code = 200,        'got ' || r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'GET /users/7/posts/99 → body NULL',            r.body IS NULL,             coalesce(r.body,'<null>') FROM r
  UNION ALL
  -- Split on ': ' (colon-space after key) to isolate numeric values in dict literals
  SELECT 'GET /users/7/posts/99 → handler_sql has 7',
         array_length(list_filter(string_split(r.handler_sql, ': '), lambda t: starts_with(t, '7'))) > 0,
         coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  SELECT 'GET /users/7/posts/99 → handler_sql has 99',
         array_length(list_filter(string_split(r.handler_sql, ': '), lambda t: starts_with(t, '99'))) > 0,
         coalesce(r.handler_sql,'<null>') FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 11  GET /openapi.json  → 200, valid OpenAPI JSON, body not null, handler_sql null
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/openapi.json','{}','')),
     parsed AS (
       SELECT
         r.status_code,
         r.content_type,
         r.body,
         r.handler_sql,
         json_extract_string(r.body, '$.openapi')   AS oai_version,
         json_extract_string(r.body, '$.info.title') AS oai_title,
         json_type(json_extract(r.body, '$.paths')) AS paths_type
       FROM r
     )
INSERT INTO _test_results
  SELECT 'GET /openapi.json → status 200',           p.status_code = 200,           'got ' || p.status_code::VARCHAR FROM parsed p
  UNION ALL
  SELECT 'GET /openapi.json → body not null',         p.body IS NOT NULL,            'body is null' FROM parsed p
  UNION ALL
  SELECT 'GET /openapi.json → handler_sql null',      p.handler_sql IS NULL,         coalesce(p.handler_sql,'<null>') FROM parsed p
  UNION ALL
  SELECT 'GET /openapi.json → content_type json',     p.content_type = 'application/json', p.content_type FROM parsed p
  UNION ALL
  SELECT 'GET /openapi.json → $.openapi = 3.0.0',     p.oai_version = '3.0.0',      coalesce(p.oai_version,'<null>') FROM parsed p
  UNION ALL
  SELECT 'GET /openapi.json → $.info.title = quackapi', p.oai_title = 'quackapi',   coalesce(p.oai_title,'<null>') FROM parsed p
  UNION ALL
  SELECT 'GET /openapi.json → $.paths is object',      p.paths_type = 'OBJECT',     coalesce(p.paths_type,'<null>') FROM parsed p;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 12  GET /docs  → 200, text/html, body contains swagger-ui, handler_sql null
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/docs','{}',''))
INSERT INTO _test_results
  SELECT 'GET /docs → status 200',              r.status_code = 200,           'got ' || r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'GET /docs → content_type text/html',  r.content_type = 'text/html',  r.content_type FROM r
  UNION ALL
  SELECT 'GET /docs → body not null',            r.body IS NOT NULL,            'body is null' FROM r
  UNION ALL
  SELECT 'GET /docs → handler_sql null',         r.handler_sql IS NULL,         coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  SELECT 'GET /docs → body has swagger-ui',
         -- check for the swagger-ui div marker (no LIKE; split on id= token)
         array_length(list_filter(string_split(r.body,'id='), lambda t: starts_with(t, '"swagger-ui"'))) > 0,
         substr(r.body,1,80) FROM r
  UNION ALL
  SELECT 'GET /docs → body has DOCTYPE',
         starts_with(r.body, '<!DOCTYPE html>'),
         substr(r.body,1,20) FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 13  GET /search?q=hi  (limit omitted — optional param)  → 200 dynamic
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/search?q=hi','{}',''))
INSERT INTO _test_results
  SELECT 'GET /search no limit → status 200',          r.status_code = 200,         'got ' || r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'GET /search no limit → handler_sql not null', r.handler_sql IS NOT NULL,   coalesce(r.handler_sql,'<null>') FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 14  GET /search (missing required q)  → 422 missing
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/search?limit=5','{}','')),
     parsed AS (
       SELECT
         r.status_code,
         json_extract_string(json_extract(r.body,'$.detail[0]'), '$.type') AS err_type,
         json_extract_string(json_extract(r.body,'$.detail[0]'), '$.loc[1]') AS err_field
       FROM r
     )
INSERT INTO _test_results
  SELECT 'GET /search missing q → status 422',   p.status_code = 422,       'got ' || p.status_code::VARCHAR FROM parsed p
  UNION ALL
  SELECT 'GET /search missing q → err missing',  p.err_type = 'missing',    coalesce(p.err_type,'<null>') FROM parsed p
  UNION ALL
  SELECT 'GET /search missing q → field is q',   p.err_field = 'q',         coalesce(p.err_field,'<null>') FROM parsed p;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 15  GET /search?q=hi&limit=abc  → 422 int_parsing on limit
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/search?q=hi&limit=abc','{}','')),
     parsed AS (
       SELECT
         r.status_code,
         json_extract_string(json_extract(r.body,'$.detail[0]'), '$.type') AS err_type,
         json_extract_string(json_extract(r.body,'$.detail[0]'), '$.loc[1]') AS err_field
       FROM r
     )
INSERT INTO _test_results
  SELECT 'GET /search limit=abc → status 422',       p.status_code = 422,          'got ' || p.status_code::VARCHAR FROM parsed p
  UNION ALL
  SELECT 'GET /search limit=abc → err int_parsing',   p.err_type = 'int_parsing',   coalesce(p.err_type,'<null>') FROM parsed p
  UNION ALL
  SELECT 'GET /search limit=abc → field is limit',    p.err_field = 'limit',        coalesce(p.err_field,'<null>') FROM parsed p;

-- ─────────────────────────────────────────────────────────────────────────────
-- CHECK 16  GET /whoami  → 200 dynamic, handler_sql references whoami
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('GET','/whoami','{}',''))
INSERT INTO _test_results
  SELECT 'GET /whoami → status 200',          r.status_code = 200,          'got ' || r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'GET /whoami → body NULL',            r.body IS NULL,               coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'GET /whoami → handler_sql not null', r.handler_sql IS NOT NULL,    coalesce(r.handler_sql,'<null>') FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- SUMMARY — print all results, then a pass/fail aggregate
-- ─────────────────────────────────────────────────────────────────────────────
SELECT check_name, pass, detail FROM _test_results ORDER BY pass ASC, check_name;

SELECT
  array_length(array_agg(check_name)) AS total_checks,
  array_length(list_filter(array_agg(pass), lambda p: p = true))  AS passed,
  array_length(list_filter(array_agg(pass), lambda p: p = false)) AS failed
FROM _test_results;
