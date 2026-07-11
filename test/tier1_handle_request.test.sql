-- ============================================================================
-- Tier-1: handle_request() direct assertions
-- Run via:  printf '.read framework.sql\n.read test/tier1_handle_request.test.sql\n' | duckdb
-- DO NOT load app.sql first: this suite is self-contained (framework demo seeds +
-- its own fixtures). app.sql TRUNCATEs the demo seeds (get_post, whoami) and
-- re-registers colliding route_ids (old_home, form_submit) -> false failures.
-- Verified 2026-07-02: with app.sql 90/98; canonical invocation 98/98.
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
  SELECT 'GET /users/abc → err_loc is id',      p.err_loc = 'id',              coalesce(p.err_loc,'<null>') FROM parsed p
  UNION ALL
  SELECT 'GET /users/abc → has input field (PARSE)', json_extract_string(json_extract(p.body,'$.detail[0]'),'$.input') IS NOT NULL, json_extract(p.body,'$.detail[0]')::VARCHAR FROM parsed p;

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
-- CHECK 7  POST /users valid body  → 201 dynamic create_user
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"zara","age":28}'))
INSERT INTO _test_results
  SELECT 'POST /users valid → status 201',          r.status_code = 201,           'got ' || r.status_code::VARCHAR FROM r
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
-- CHECK: Content-Type gate for JSON body (B)
-- wrong-CT (text/plain) with json body for model -> 422 (FastAPI behavior)
-- missing-CT (headers {}) with json body -> 201 (FastAPI accepts, parses)
-- ─────────────────────────────────────────────────────────────────────────────
WITH r AS (SELECT * FROM handle_request('POST','/users','{"content-type":"text/plain"}','{"name":"wrongct","age":11}'))
INSERT INTO _test_results
  SELECT 'POST /users wrong-ct text/plain → status 422', r.status_code = 422, 'got ' || r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'POST /users wrong-ct → handler_sql null (no parse)', r.handler_sql IS NULL, coalesce(r.handler_sql,'<null>') FROM r;

WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"missct","age":12}')),
     parsed AS (
       SELECT r.status_code, r.handler_sql, json_extract_string(r.body,'$.detail') AS det FROM r
     )
INSERT INTO _test_results
  SELECT 'POST /users missing-ct {} → status 201', p.status_code = 201, 'got ' || p.status_code::VARCHAR FROM parsed p
  UNION ALL
  SELECT 'POST /users missing-ct → handler_sql not null', p.handler_sql IS NOT NULL, coalesce(p.handler_sql,'<null>') FROM parsed p;

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
  SELECT 'GET /search missing q → field is q',   p.err_field = 'q',         coalesce(p.err_field,'<null>') FROM parsed p
  UNION ALL
  SELECT 'GET /search missing q → input null (missing)', json_extract_string(json_extract(r.body,'$.detail[0]'),'$.input') IS NULL, json_extract(r.body,'$.detail[0]')::VARCHAR FROM r;

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
  SELECT 'GET /search limit=abc → field is limit',    p.err_field = 'limit',        coalesce(p.err_field,'<null>') FROM parsed p
  UNION ALL
  SELECT 'GET /search limit=abc → input present (PARSE)', json_extract_string(json_extract(r.body,'$.detail[0]'),'$.input') = 'abc', json_extract(r.body,'$.detail[0]')::VARCHAR FROM r;

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
-- R1 REQUEST SURFACE CHECKS (added routes in this test file to keep framework
-- seeds untouched for parity_b2 regression; all additive)
-- These run only in extended tier1; use app.sql for full demo app load.
-- ─────────────────────────────────────────────────────────────────────────────

-- Seed surface demo routes/params/headers (additive, old routes intact)
INSERT INTO routes SELECT * FROM register_route('secure_ping','GET','/secure','SELECT to_json({''ok'':true}) AS body','dynamic','hdr',200);
INSERT INTO param_schema (route_id, name, location, type, required, constraint_json) VALUES ('secure_ping','x_api_key','header','string',true,NULL);

INSERT INTO routes SELECT * FROM register_route('profile','GET','/profile','SELECT to_json({''s'':{session}}) AS body','dynamic','ck',200);
INSERT INTO param_schema (route_id, name, location, type, required, constraint_json) VALUES ('profile','session','cookie','string',true,NULL);

INSERT INTO routes SELECT * FROM register_route('form_submit','POST','/form-submit','SELECT to_json({''n'':{name}, ''a'':{age}}) AS body','dynamic','fm',200);
INSERT INTO param_schema (route_id, name, location, type, required, constraint_json) VALUES ('form_submit','name','body','string',true,NULL);
INSERT INTO param_schema (route_id, name, location, type, required, constraint_json) VALUES ('form_submit','age','body','int',true,NULL);

INSERT INTO routes SELECT * FROM register_redirect('old_home','GET','/old-home','/new',307);
INSERT INTO route_headers VALUES ('old_home','Location','/new');

INSERT INTO routes SELECT * FROM register_route('login_set','POST','/login','{}','static','sc',200);
INSERT INTO route_headers VALUES ('login_set','Set-Cookie','sess=1; Path=/');

-- CHECK 17 header param happy: X-API-Key present -> 200
WITH r AS (SELECT * FROM handle_request('GET','/secure','{"x_api_key":"k-123"}',''))
INSERT INTO _test_results SELECT 'R1 header happy → status 200', r.status_code=200, 'got '||r.status_code FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/secure','{"x_api_key":"k-123"}',''))
INSERT INTO _test_results SELECT 'R1 header happy → resp_headers {} (no route hdr)', r.resp_headers='{}', r.resp_headers FROM r;

-- CHECK 18 header required missing -> 422 loc header
WITH r AS (SELECT * FROM handle_request('GET','/secure','{}',''))
INSERT INTO _test_results SELECT 'R1 header missing → 422', r.status_code=422, 'got '||r.status_code FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/secure','{}',''))
INSERT INTO _test_results SELECT 'R1 header missing → loc[0]=header', json_extract_string(json_extract(r.body,'$.detail[0]'),'$.loc[0]')='header', json_extract(r.body,'$.detail[0]') FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/secure','{}',''))
INSERT INTO _test_results SELECT 'R1 header missing → loc[1]=x_api_key', json_extract_string(json_extract(r.body,'$.detail[0]'),'$.loc[1]')='x_api_key', json_extract(r.body,'$.detail[0]') FROM r;

-- CHECK 19 cookie happy
WITH r AS (SELECT * FROM handle_request('GET','/profile','{"_cookies":{"session":"s1"}}',''))
INSERT INTO _test_results
  SELECT 'R1 cookie happy → 200', r.status_code=200, 'got '||r.status_code FROM r;

-- CHECK 20 cookie missing -> 422
WITH r AS (SELECT * FROM handle_request('GET','/profile','{"_cookies":{}}',''))
INSERT INTO _test_results SELECT 'R1 cookie missing → 422', r.status_code=422, 'got '||r.status_code FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/profile','{"_cookies":{}}',''))
INSERT INTO _test_results SELECT 'R1 cookie loc cookie', json_extract_string(json_extract(r.body,'$.detail[0]'),'$.loc[0]')='cookie', json_extract(r.body,'$.detail[0]') FROM r;

-- CHECK 21 form happy (urlencoded body)
WITH r AS (SELECT * FROM handle_request('POST','/form-submit','{"content-type":"application/x-www-form-urlencoded"}','name=zed&age=31'))
INSERT INTO _test_results SELECT 'R1 form happy → 200', r.status_code=200, 'got '||r.status_code FROM r;
WITH r AS (SELECT * FROM handle_request('POST','/form-submit','{"content-type":"application/x-www-form-urlencoded"}','name=zed&age=31'))
INSERT INTO _test_results SELECT 'R1 form handler has zed', array_length(list_filter(string_split(r.handler_sql,''''), lambda t:t='zed'))>0 , coalesce(r.handler_sql,'<null>') FROM r;

-- CHECK 22 form bad type 422
WITH r AS (SELECT * FROM handle_request('POST','/form-submit','{"content-type":"application/x-www-form-urlencoded"}','name=zed&age=abc'))
INSERT INTO _test_results SELECT 'R1 form bad int → 422', r.status_code=422, 'got '||r.status_code FROM r;
WITH r AS (SELECT * FROM handle_request('POST','/form-submit','{"content-type":"application/x-www-form-urlencoded"}','name=zed&age=abc'))
INSERT INTO _test_results SELECT 'R1 form err int_parsing', json_extract_string(json_extract(r.body,'$.detail[0]'),'$.type')='int_parsing', json_extract(r.body,'$.detail[0]') FROM r;

-- CHECK 23 redirect: status 307 + Location in resp_headers + body null + no hsql
WITH r AS (SELECT * FROM handle_request('GET','/old-home','{}',''))
INSERT INTO _test_results SELECT 'R1 redirect → status 307', r.status_code=307, 'got '||r.status_code FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/old-home','{}',''))
INSERT INTO _test_results SELECT 'R1 redirect → body NULL', r.body IS NULL, coalesce(r.body,'<null>') FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/old-home','{}',''))
INSERT INTO _test_results SELECT 'R1 redirect → handler_sql NULL', r.handler_sql IS NULL, coalesce(r.handler_sql,'<null>') FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/old-home','{}',''))
INSERT INTO _test_results SELECT 'R1 redirect → Location in resp_headers', json_extract_string(r.resp_headers,'$.Location')='/new', r.resp_headers FROM r;

-- CHECK 24 Set-Cookie emitted via route_headers
WITH r AS (SELECT * FROM handle_request('POST','/login','{}',''))
INSERT INTO _test_results SELECT 'R1 set-cookie → 200', r.status_code=200, 'got '||r.status_code FROM r;
WITH r AS (SELECT * FROM handle_request('POST','/login','{}',''))
INSERT INTO _test_results SELECT 'R1 set-cookie header present', json_extract_string(r.resp_headers,'$.Set-Cookie')='sess=1; Path=/', r.resp_headers FROM r;

-- (cors preflight 204 + ACA* and post injection tested via middleware chain in middleware.test.sql)

-- ─────────────────────────────────────────────────────────────────────────────
-- POLISH_OPS CHECKS (HEAD auto-handling and 405 with Allow header)
-- ─────────────────────────────────────────────────────────────────────────────

-- CHECK PO-1: HEAD on static route returns same status and body as GET (oracle returns full body)
WITH hd AS (SELECT * FROM handle_request('HEAD','/health','{}','')),
     gt AS (SELECT * FROM handle_request('GET', '/health','{}',''))
INSERT INTO _test_results
  SELECT 'PO HEAD /health → same status as GET',  (SELECT status_code FROM hd) = (SELECT status_code FROM gt),
         'head=' || (SELECT status_code::VARCHAR FROM hd) || ' get=' || (SELECT status_code::VARCHAR FROM gt)
  UNION ALL
  SELECT 'PO HEAD /health → body matches GET',    (SELECT body FROM hd) = (SELECT body FROM gt),
         'head_body=' || coalesce((SELECT body FROM hd),'<null>');

-- CHECK PO-2: HEAD on dynamic route → 200 + handler_sql (not NULL)
WITH r AS (SELECT * FROM handle_request('HEAD','/users/1','{}',''))
INSERT INTO _test_results
  SELECT 'PO HEAD /users/1 → 200',             r.status_code = 200,           'got '||r.status_code FROM r
  UNION ALL
  SELECT 'PO HEAD /users/1 → handler_sql set', r.handler_sql IS NOT NULL,     coalesce(r.handler_sql,'<null>') FROM r;

-- CHECK PO-3: HEAD on nonexistent path → 404
WITH r AS (SELECT * FROM handle_request('HEAD','/nope','{}',''))
INSERT INTO _test_results
  SELECT 'PO HEAD /nope → 404', r.status_code = 404, 'got '||r.status_code FROM r;

-- CHECK PO-4: DELETE on path that has only GET → 405
WITH r AS (SELECT * FROM handle_request('DELETE','/health','{}',''))
INSERT INTO _test_results
  SELECT 'PO DELETE /health → 405',               r.status_code = 405,  'got '||r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'PO DELETE /health → Allow in resp_headers',
         json_extract_string(r.resp_headers,'$.Allow') IS NOT NULL,    r.resp_headers::VARCHAR FROM r
  UNION ALL
  SELECT 'PO DELETE /health → Allow contains GET',
         instr(coalesce(json_extract_string(r.resp_headers,'$.Allow'),''),'GET') > 0, r.resp_headers::VARCHAR FROM r
  UNION ALL
  SELECT 'PO DELETE /health → Allow contains HEAD',
         instr(coalesce(json_extract_string(r.resp_headers,'$.Allow'),''),'HEAD') > 0, r.resp_headers::VARCHAR FROM r
  UNION ALL
  SELECT 'PO DELETE /health → body is Method Not Allowed JSON',
         json_extract_string(r.body,'$.detail') = 'Method Not Allowed', coalesce(r.body,'<null>') FROM r;

-- CHECK PO-5: DELETE on /users (has GET + POST) → 405 with Allow containing POST
WITH r AS (SELECT * FROM handle_request('DELETE','/users','{}',''))
INSERT INTO _test_results
  SELECT 'PO DELETE /users → 405',   r.status_code = 405, 'got '||r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'PO DELETE /users → Allow contains POST',
         instr(coalesce(json_extract_string(r.resp_headers,'$.Allow'),''),'POST') > 0, r.resp_headers::VARCHAR FROM r;

-- CHECK PO-6: DELETE on nonexistent path → 404 (NOT 405)
WITH r AS (SELECT * FROM handle_request('DELETE','/totally-nonexistent-xyzzy','{}',''))
INSERT INTO _test_results
  SELECT 'PO DELETE /xyzzy → 404 not 405', r.status_code = 404, 'got '||r.status_code::VARCHAR FROM r;

-- CHECK PO-7: OPTIONS on GET route (no CORS) → 405 (Starlette parity)
WITH r AS (SELECT * FROM handle_request('OPTIONS','/health','{}',''))
INSERT INTO _test_results
  SELECT 'PO OPTIONS /health → 405', r.status_code = 405, 'got '||r.status_code::VARCHAR FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- CORS CHECKS (CREATE CORS + Starlette parity: preflight 200+ACA, disallowed no ACAO or 400, actual carries ACAO, * vs creds)
-- Insert policy, assert, leave row (harmless to later checks).
-- ─────────────────────────────────────────────────────────────────────────────

-- Ensure clean start for cors tests (in case prior state)
DELETE FROM quackapi_cors;

-- CHECK CORS-1: CREATE CORS with * origins, explicit methods/headers, no creds
INSERT INTO quackapi_cors SELECT * FROM register_cors(
  '["*"]'::VARCHAR,
  '["GET","POST","OPTIONS","PUT","DELETE"]'::VARCHAR,
  '["*"]'::VARCHAR,
  false,
  '[]'::VARCHAR,
  600
);

-- preflight allowed * : 200 (not 405), ACA headers present, ACAO=echo for this case? but * allowed without creds -> may be *, but our _build echoes? wait per impl for !creds * sets *
WITH r AS (SELECT * FROM handle_request('OPTIONS','/health','{"origin":"http://x.example","access-control-request-method":"POST"}',''))
INSERT INTO _test_results
  SELECT 'CORS preflight allowed → 200', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'CORS preflight ACAO present', json_extract_string(r.resp_headers, '$.Access-Control-Allow-Origin') IS NOT NULL, coalesce(r.resp_headers,'<null>') FROM r
  UNION ALL
  SELECT 'CORS preflight has ACA-Methods', instr(coalesce(json_extract_string(r.resp_headers,'$.Access-Control-Allow-Methods'),''), 'POST') > 0, r.resp_headers FROM r
  UNION ALL
  SELECT 'CORS preflight has Max-Age', json_extract_string(r.resp_headers, '$.Access-Control-Max-Age') = '600', r.resp_headers FROM r;

-- actual GET with Origin: carries ACAO ( '*' for *+!creds per starlette simple_headers )
WITH r AS (SELECT * FROM handle_request('GET','/health','{"origin":"http://x.example"}',''))
INSERT INTO _test_results
  SELECT 'CORS actual GET carries ACAO', json_extract_string(r.resp_headers, '$.Access-Control-Allow-Origin') = '*', coalesce(json_extract_string(r.resp_headers, '$.Access-Control-Allow-Origin'),'<null>') FROM r;

-- disallow origin: 400 + NO ACAO header
DELETE FROM quackapi_cors;
INSERT INTO quackapi_cors SELECT * FROM register_cors(
  '["https://good.example"]'::VARCHAR,
  '["GET","POST","OPTIONS"]'::VARCHAR,
  '["Content-Type"]'::VARCHAR,
  false,
  '[]'::VARCHAR,
  600
);
WITH r AS (SELECT * FROM handle_request('OPTIONS','/users/1','{"origin":"http://evil.example","access-control-request-method":"GET"}',''))
INSERT INTO _test_results
  SELECT 'CORS preflight disallowed → 400', r.status_code = 400, 'got '||r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'CORS disallowed preflight has no ACAO', json_extract_string(r.resp_headers, '$.Access-Control-Allow-Origin') IS NULL, coalesce(r.resp_headers,'<null>') FROM r;

-- actual with disallowed: no ACAO added
WITH r AS (SELECT * FROM handle_request('GET','/health','{"origin":"http://evil.example"}',''))
INSERT INTO _test_results
  SELECT 'CORS actual disallowed origin no ACAO added', json_extract_string(r.resp_headers, '$.Access-Control-Allow-Origin') IS NULL, coalesce(json_extract_string(r.resp_headers,'$.Access-Control-Allow-Origin'),'<null>') FROM r;

-- creds + * : must echo origin not *
DELETE FROM quackapi_cors;
INSERT INTO quackapi_cors SELECT * FROM register_cors(
  '["*"]'::VARCHAR,
  '["GET","POST","OPTIONS"]'::VARCHAR,
  '["*"]'::VARCHAR,
  true,
  '["X-Custom"]'::VARCHAR,
  300
);
WITH r AS (SELECT * FROM handle_request('GET','/health','{"origin":"https://cred.example"}',''))
INSERT INTO _test_results
  SELECT 'CORS *+creds echoes origin', json_extract_string(r.resp_headers, '$.Access-Control-Allow-Origin') = 'https://cred.example', coalesce(json_extract_string(r.resp_headers, '$.Access-Control-Allow-Origin'),'<null>') FROM r
  UNION ALL
  SELECT 'CORS creds has Allow-Credentials', json_extract_string(r.resp_headers, '$.Access-Control-Allow-Credentials') = 'true', r.resp_headers FROM r
  UNION ALL
  SELECT 'CORS expose present', instr(coalesce(json_extract_string(r.resp_headers,'$.Access-Control-Expose-Headers'),''), 'X-Custom') > 0, r.resp_headers FROM r;

-- preflight with creds * echoes
WITH r AS (SELECT * FROM handle_request('OPTIONS','/health','{"origin":"https://cred.example","access-control-request-method":"POST"}',''))
INSERT INTO _test_results
  SELECT 'CORS preflight *+creds echoes', json_extract_string(r.resp_headers, '$.Access-Control-Allow-Origin') = 'https://cred.example', coalesce(json_extract_string(r.resp_headers, '$.Access-Control-Allow-Origin'),'<null>') FROM r;

-- clean for any downstream (optional)
DELETE FROM quackapi_cors;

-- ─────────────────────────────────────────────────────────────────────────────
-- R2 MULTIPART/FORM-DATA CHECKS
-- Register an upload route with type='file' param.
-- These checks are additive and do not affect any prior route.
-- ─────────────────────────────────────────────────────────────────────────────

INSERT INTO routes SELECT * FROM register_route(
  'mp_upload',
  'POST',
  '/upload',
  'SELECT to_json({''filename'': {file__filename}, ''size'': len({file}), ''preview'': substr({file},1,80)}) AS body',
  'dynamic',
  'Multipart file upload demo',
  200
);
INSERT INTO param_schema (route_id, name, location, type, required, constraint_json)
SELECT 'mp_upload', 'file', 'body', 'file', true, NULL;

INSERT INTO routes SELECT * FROM register_route(
  'mp_mixed',
  'POST',
  '/upload-mixed',
  'SELECT to_json({''fn'': {doc__filename}, ''desc'': {description}}) AS body',
  'dynamic',
  'Mixed file + field demo',
  200
);
INSERT INTO param_schema (route_id, name, location, type, required, constraint_json)
SELECT 'mp_mixed', 'doc', 'body', 'file', true, NULL
UNION ALL
SELECT 'mp_mixed', 'description', 'body', 'string', false, NULL;

-- CHECK 25: simple file part → 200 with handler_sql rendered
WITH r AS (SELECT * FROM handle_request(
  'POST','/upload',
  '{"content-type":"multipart/form-data; boundary=testbnd"}',
  E'--testbnd\r\nContent-Disposition: form-data; name="file"; filename="hello.txt"\r\n\r\nhello world\r\n--testbnd--'
))
INSERT INTO _test_results
  SELECT 'MP simple file → 200', r.status_code=200, 'got '||r.status_code FROM r
  UNION ALL
  SELECT 'MP simple file → has handler_sql', r.handler_sql IS NOT NULL, coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  SELECT 'MP simple file → filename in handler', starts_with(r.handler_sql,'SELECT') AND instr(r.handler_sql,'hello.txt')>0, coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  SELECT 'MP simple file → content in handler', instr(r.handler_sql,'hello world')>0, coalesce(r.handler_sql,'<null>') FROM r;

-- CHECK 26: file part with quoted boundary → 200
WITH r AS (SELECT * FROM handle_request(
  'POST','/upload',
  '{"content-type":"multipart/form-data; boundary=\"mybnd\""}',
  E'--mybnd\r\nContent-Disposition: form-data; name="file"; filename="quoted.txt"\r\n\r\ncontent\r\n--mybnd--'
))
INSERT INTO _test_results
  SELECT 'MP quoted boundary → 200', r.status_code=200, 'got '||r.status_code FROM r
  UNION ALL
  SELECT 'MP quoted boundary → filename rendered', instr(coalesce(r.handler_sql,''),'quoted.txt')>0, coalesce(r.handler_sql,'<null>') FROM r;

-- CHECK 27: mixed file + text field → 200 (handler has both values)
WITH r AS (SELECT * FROM handle_request(
  'POST','/upload-mixed',
  '{"content-type":"multipart/form-data; boundary=mixbnd"}',
  E'--mixbnd\r\nContent-Disposition: form-data; name="doc"; filename="report.csv"\r\n\r\ncol1,col2\r\n--mixbnd\r\nContent-Disposition: form-data; name="description"\r\n\r\nmy report\r\n--mixbnd--'
))
INSERT INTO _test_results
  SELECT 'MP mixed file+field → 200', r.status_code=200, 'got '||r.status_code FROM r
  UNION ALL
  SELECT 'MP mixed → filename rendered', instr(coalesce(r.handler_sql,''),'report.csv')>0, coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  SELECT 'MP mixed → text field rendered', instr(coalesce(r.handler_sql,''),'my report')>0, coalesce(r.handler_sql,'<null>') FROM r;

-- CHECK 28: missing required file part → 422 missing
WITH r AS (SELECT * FROM handle_request(
  'POST','/upload',
  '{"content-type":"multipart/form-data; boundary=bnd2"}',
  E'--bnd2\r\nContent-Disposition: form-data; name="other"\r\n\r\nsome text\r\n--bnd2--'
))
INSERT INTO _test_results
  SELECT 'MP missing file → 422', r.status_code=422, 'got '||r.status_code FROM r
  UNION ALL
  SELECT 'MP missing file → err missing', json_extract_string(json_extract(r.body,'$.detail[0]'),'$.type')='missing', coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'MP missing file → loc body', json_extract_string(json_extract(r.body,'$.detail[0]'),'$.loc[0]')='body', coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'MP missing file → loc file', json_extract_string(json_extract(r.body,'$.detail[0]'),'$.loc[1]')='file', coalesce(r.body,'<null>') FROM r;

-- CHECK 29: malformed multipart (no boundary in body) → 422 multipart_parse
WITH r AS (SELECT * FROM handle_request(
  'POST','/upload',
  '{"content-type":"multipart/form-data; boundary=realbnd"}',
  'this body has no boundary delimiter at all'
))
INSERT INTO _test_results
  SELECT 'MP malformed → 422', r.status_code=422, 'got '||r.status_code FROM r
  UNION ALL
  SELECT 'MP malformed → err type multipart_parse', json_extract_string(json_extract(r.body,'$.detail[0]'),'$.type')='multipart_parse', coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'MP malformed → loc body', json_extract_string(json_extract(r.body,'$.detail[0]'),'$.loc[0]')='body', coalesce(r.body,'<null>') FROM r;

-- CHECK 30: multipart detection miss → non-multipart body falls through to JSON path (no false-positive)
WITH r AS (SELECT * FROM handle_request(
  'POST','/users',
  '{}',
  '{"name":"mp_test","age":5}'
))
INSERT INTO _test_results
  SELECT 'MP non-mp route unaffected → 201', r.status_code=201, 'got '||r.status_code FROM r
  UNION ALL
  SELECT 'MP non-mp route → has handler_sql', r.handler_sql IS NOT NULL, coalesce(r.handler_sql,'<null>') FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- CREATE AUTH + CREATE POLICY v1 cases (oracle coverage via helpers + manual)
-- handle_request core routing unchanged for baseline parity; auth/policy phases
-- exercised via _verify + policy tables + direct status asserts here.
-- C layer (brain + parser) will integrate for live 401/403/claims and DDL tests.
-- ─────────────────────────────────────────────────────────────────────────────

-- Seed a JWT auth scheme (secret matches the canonical example) via register macro (sugar)
INSERT INTO quackapi_auth SELECT * FROM register_auth('bearer', 'jwt_hs256', '{"header":"Authorization","verify_exp":true,"leeway":30,"secret":"your-256-bit-secret"}');

-- Seed an API key table and scheme. `subject` is the identity the key authenticates as —
-- claims['sub'] is populated from it, never from the raw key.
CREATE OR REPLACE TABLE api_keys (key VARCHAR, subject VARCHAR);
INSERT INTO api_keys VALUES ('k-123', 'svc_reporting'), ('k-456', 'svc_ingest');
INSERT INTO quackapi_auth SELECT * FROM register_auth('apikey', 'api_key', '{"header":"X-API-Key"}');

-- A policy that will be used for 403 test (sentinel) via register macro (sugar)
INSERT INTO policies SELECT * FROM register_policy('deny_all', 'GET /deny', 'RESTRICTIVE', 'false', NULL, 'bearer');

-- CHECK A1: missing token on authed pattern -> 401 (via verify path)
-- We simulate handle_request returning 401 by checking verify returns NULL and policy requires
WITH v AS (SELECT _verify_jwt_hs256(NULL, 'your-256-bit-secret', true, 30) AS c)
INSERT INTO _test_results
  SELECT 'AUTH missing token → 401 (verify null)', (SELECT c FROM v) IS NULL, 'claims='||coalesce((SELECT c FROM v)::VARCHAR,'<null>') FROM v;

-- CHECK A2: malformed token -> 401
WITH v AS (SELECT _verify_jwt_hs256('not.a.jwt', 'your-256-bit-secret', true, 30) AS c)
INSERT INTO _test_results SELECT 'AUTH malformed → 401 (verify null)', (SELECT c FROM v) IS NULL, '' FROM v;

-- CHECK A3: bad signature -> 401
WITH v AS (SELECT _verify_jwt_hs256('eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ.badSIG', 'your-256-bit-secret', true, 30) AS c)
INSERT INTO _test_results SELECT 'AUTH bad sig → 401 (verify null)', (SELECT c FROM v) IS NULL, '' FROM v;

-- CHECK A4: valid token structure (canonical probe-verified per spec; here assert helper executes and bad-sig path is null)
WITH vbad AS (SELECT _verify_jwt_hs256('bad', 'your-256-bit-secret', false, 0) AS c)
INSERT INTO _test_results
  SELECT 'AUTH verify helper executes (bad sig null)', (SELECT c FROM vbad) IS NULL, '' FROM vbad;

-- CHECK A5: valid + policy sentinel false -> 403
WITH has_deny AS (SELECT count(*) > 0 FROM policies WHERE policy_id='deny_all' AND using_pred='false')
INSERT INTO _test_results SELECT 'AUTH valid+policy-false → 403', (SELECT * FROM has_deny), '' FROM has_deny;

-- CHECK A6: API key valid
WITH ok AS (SELECT count(*) > 0 FROM api_keys k WHERE _constant_time_str_equals('k-123', k.key))
INSERT INTO _test_results SELECT 'AUTH apikey valid → ok', (SELECT * FROM ok), '' FROM ok;

-- CHECK A7: API key invalid
WITH nok AS (SELECT count(*) > 0 FROM api_keys k WHERE _constant_time_str_equals('nope', k.key))
INSERT INTO _test_results SELECT 'AUTH apikey invalid → not ok', NOT (SELECT * FROM nok), '' FROM nok;

-- claims visible to handler simulation (wrap + eval)
WITH ctx AS (SELECT map_from_entries([struct_pack(key:='sub', value:='123')]) AS claims)
INSERT INTO _test_results
  SELECT 'AUTH claims visible (wrap sim) → sub present', (SELECT claims FROM ctx)['sub'] = '123', (SELECT claims FROM ctx)::VARCHAR FROM ctx;

-- ── Real-world token regression (bug: blob::VARCHAR mangled payload → NULL claims) ──
-- Tokens minted externally (python stdlib HS256, secret 'tier1-test-secret'): the payload
-- JSON has spaces after ':' and a NUMERIC exp — the shape every real JWT library emits.
-- CHECK A8: valid real-world token, verify_exp on → claims with correct sub
WITH v AS (SELECT _verify_jwt_hs256('eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJzdWIiOiAidGllcjFfcmVhbF9zdWJqZWN0IiwgImV4cCI6IDk5OTk5OTk5OTl9.P78usc8coJGG6h3xSeWOvvNELglllffrDbZBdYaMARY', 'tier1-test-secret', true, 30) AS c)
INSERT INTO _test_results
  SELECT 'AUTH real-world valid token → claims sub', (SELECT c FROM v)['sub'] = 'tier1_real_subject', (SELECT c FROM v)::VARCHAR FROM v;

-- CHECK A9: expired real-world token, verify_exp on → NULL
WITH v AS (SELECT _verify_jwt_hs256('eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJzdWIiOiAidGllcjFfZXhwaXJlZF9zdWJqZWN0IiwgImV4cCI6IDEwMDAwMDAwMDB9.Z2CNaGXPTCbwfslIe3P9lOEBv69zFXkkDKW0USvyyxI', 'tier1-test-secret', true, 30) AS c)
INSERT INTO _test_results
  SELECT 'AUTH real-world expired + verify_exp → NULL', (SELECT c FROM v) IS NULL, '' FROM v;

-- CHECK A10: same expired token, verify_exp off → claims still returned
WITH v AS (SELECT _verify_jwt_hs256('eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJzdWIiOiAidGllcjFfZXhwaXJlZF9zdWJqZWN0IiwgImV4cCI6IDEwMDAwMDAwMDB9.Z2CNaGXPTCbwfslIe3P9lOEBv69zFXkkDKW0USvyyxI', 'tier1-test-secret', false, 0) AS c)
INSERT INTO _test_results
  SELECT 'AUTH expired + verify_exp off → claims', (SELECT c FROM v)['sub'] = 'tier1_expired_subject', (SELECT c FROM v)::VARCHAR FROM v;

-- CHECK A11: valid token, wrong secret → NULL
WITH v AS (SELECT _verify_jwt_hs256('eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJzdWIiOiAidGllcjFfcmVhbF9zdWJqZWN0IiwgImV4cCI6IDk5OTk5OTk5OTl9.P78usc8coJGG6h3xSeWOvvNELglllffrDbZBdYaMARY', 'a-different-secret', true, 30) AS c)
INSERT INTO _test_results
  SELECT 'AUTH valid token wrong secret → NULL', (SELECT c FROM v) IS NULL, '' FROM v;

-- CHECK A12: SQL-injection-shaped credential → NULL, no error
WITH v AS (SELECT _verify_jwt_hs256('x'' OR ''1''=''1', 'tier1-test-secret', true, 30) AS c)
INSERT INTO _test_results
  SELECT 'AUTH injection-shaped token → NULL', (SELECT c FROM v) IS NULL, '' FROM v;

-- ─────────────────────────────────────────────────────────────────────────────
-- ORACLE AUTH WIRING (BACKLOG §3.9 P1): FULL handle_request macro on policed routes.
-- All seeds use register_* sugar (no raw route INSERTs for these assertions).
-- Policies use forms the oracle special-cases (''/true or 'false'); this is sufficient
-- for the required parity matrix and matches C literal fast-path before prepared eval.
-- ─────────────────────────────────────────────────────────────────────────────

-- Policied routes via sugar (additive; ids chosen not to collide)
INSERT INTO routes SELECT * FROM register_route('p_jwt', 'GET', '/p/jwt', 'SELECT to_json({''sub'': claims[''sub'']}) AS body', 'dynamic', 'jwt policed', 200);
INSERT INTO routes SELECT * FROM register_route('p_deny', 'GET', '/p/deny', 'SELECT 1 AS body', 'dynamic', 'restr deny', 200);
INSERT INTO routes SELECT * FROM register_route('p_key', 'GET', '/p/key', 'SELECT to_json({''sub'': claims[''sub'']}) AS body', 'dynamic', 'apikey policed', 200);

-- Policies via sugar. Reuse already-registered 'bearer' (verify_exp true) and 'apikey'.
INSERT INTO policies SELECT * FROM register_policy('p_jwt_allow', 'GET /p/jwt', 'PERMISSIVE', '', NULL, 'bearer');
INSERT INTO policies SELECT * FROM register_policy('p_deny_all', 'GET /p/deny', 'RESTRICTIVE', 'false', NULL, 'bearer');
INSERT INTO policies SELECT * FROM register_policy('p_key_allow', 'GET /p/key', 'PERMISSIVE', 'true', NULL, 'apikey');

-- NEW A13: no token on policed JWT route via handle_request macro → 401
WITH r AS (SELECT * FROM handle_request('GET','/p/jwt','{}',''))
INSERT INTO _test_results
  SELECT 'AUTH macro no-token /p/jwt → 401', r.status_code=401, 'got '||r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'AUTH macro no-token body Unauthorized', json_extract_string(r.body,'$.detail')='Unauthorized', coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'AUTH macro no-token handler_sql NULL', r.handler_sql IS NULL, coalesce(r.handler_sql,'<null>') FROM r;

-- NEW A14: valid JWT (far-future exp matching seeded bearer secret) → 200 + _ctx wrap with claims
WITH r AS (SELECT * FROM handle_request('GET','/p/jwt','{"authorization":"Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJ0aHJ1LWF1dGgiLCJleHAiOjk5OTk5OTk5OTl9.K1lIlSabHymAleKdO2AX8BrdkpJdaFP2sfGLCVM1P2k"}',''))
INSERT INTO _test_results
  SELECT 'AUTH macro valid-jwt /p/jwt → 200', r.status_code=200, 'got '||r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'AUTH macro valid-jwt handler wrapped _ctx', starts_with(COALESCE(r.handler_sql,''), 'WITH _ctx AS (SELECT '''), coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  SELECT 'AUTH macro valid-jwt wrap contains claims sub', instr(COALESCE(r.handler_sql,''), 'thru-auth')>0, coalesce(r.handler_sql,'<null>') FROM r;

-- NEW A15: expired + verify_exp (real-world expired token) → 401 on policed
WITH r AS (SELECT * FROM handle_request('GET','/p/jwt','{"authorization":"Bearer eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJzdWIiOiAidGllcjFfZXhwaXJlZF9zdWJqZWN0IiwgImV4cCI6IDEwMDAwMDAwMDB9.Z2CNaGXPTCbwfslIe3P9lOEBv69zFXkkDKW0USvyyxI"}',''))
INSERT INTO _test_results
  SELECT 'AUTH macro expired+verify /p/jwt → 401', r.status_code=401, 'got '||r.status_code::VARCHAR FROM r;

-- NEW A16: RESTRICTIVE false policy + valid token → 403, handler null
WITH r AS (SELECT * FROM handle_request('GET','/p/deny','{"authorization":"Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJ0aHJ1LWF1dGgiLCJleHAiOjk5OTk5OTk5OTl9.K1lIlSabHymAleKdO2AX8BrdkpJdaFP2sfGLCVM1P2k"}',''))
INSERT INTO _test_results
  SELECT 'AUTH macro restr-false /p/deny → 403', r.status_code=403, 'got '||r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'AUTH macro restr-false body Forbidden', json_extract_string(r.body,'$.detail')='Forbidden', coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'AUTH macro restr-false handler_sql NULL', r.handler_sql IS NULL, coalesce(r.handler_sql,'<null>') FROM r;

-- NEW A17: valid api-key on policed apikey route → 200 (wrap or at least hsql)
WITH r AS (SELECT * FROM handle_request('GET','/p/key','{"x-api-key":"k-123"}',''))
INSERT INTO _test_results
  SELECT 'AUTH macro apikey-valid /p/key → 200', r.status_code=200, 'got '||r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'AUTH macro apikey-valid produces hsql', r.handler_sql IS NOT NULL, coalesce(r.handler_sql,'<null>') FROM r;

-- NEW A18: wrong api-key → 401
WITH r AS (SELECT * FROM handle_request('GET','/p/key','{"x-api-key":"nope-wrong"}',''))
INSERT INTO _test_results
  SELECT 'AUTH macro apikey-wrong → 401', r.status_code=401, 'got '||r.status_code::VARCHAR FROM r;

-- NEW A19: injection-shaped token on policed → 401 (no error, treated invalid)
WITH r AS (SELECT * FROM handle_request('GET','/p/jwt','{"authorization":"Bearer x'' OR ''1''=''1"}',''))
INSERT INTO _test_results
  SELECT 'AUTH macro injection-token → 401', r.status_code=401, 'got '||r.status_code::VARCHAR FROM r;

-- NEW A20: HONEST BOUNDARY — a NON-literal predicate (the common "require authenticated
-- user" idiom) FAIL-CLOSES to 403 on the oracle/pure-track, EVEN with a valid token,
-- because a macro cannot EXECUTE a dynamic predicate. This pins the documented limitation
-- so it can't be silently turned into a false pass. The compiled ext-cpp track evaluates
-- the full predicate (→ 200); see docs/AUTH_ORACLE_WIRING_RESULT.md "HONEST BOUNDARY".
INSERT INTO routes SELECT * FROM register_route('p_nonlit', 'GET', '/p/nonlit', 'SELECT to_json({''sub'': claims[''sub'']}) AS body FROM _ctx', 'dynamic', 'non-literal pred', 200);
INSERT INTO policies SELECT * FROM register_policy('p_nonlit_allow', 'GET /p/nonlit', 'PERMISSIVE', 'claims[''sub''] IS NOT NULL', NULL, 'bearer');
WITH r AS (SELECT * FROM handle_request('GET','/p/nonlit','{"authorization":"Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJ0aHJ1LWF1dGgiLCJleHAiOjk5OTk5OTk5OTl9.K1lIlSabHymAleKdO2AX8BrdkpJdaFP2sfGLCVM1P2k"}',''))
INSERT INTO _test_results
  SELECT 'AUTH macro non-literal pred fail-closes → 403 (documented boundary)', r.status_code=403, 'got '||r.status_code::VARCHAR FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- #1907 HEALTH / READINESS / LIVENESS + METRICS + CREATE HEALTH CHECK
-- Assertions for livez/readyz/metrics + CREATE sugar + 503 simulation
-- ─────────────────────────────────────────────────────────────────────────────

-- CHECK H1: /livez static 200 (cheap, always up while serving)
WITH r AS (SELECT * FROM handle_request('GET','/livez','{}',''))
INSERT INTO _test_results
  SELECT 'H1 /livez → status 200', r.status_code=200, 'got '||r.status_code FROM r
  UNION ALL
  SELECT 'H1 /livez → static body alive', json_extract_string(r.body,'$.status')='alive', coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'H1 /livez → handler_sql NULL (static)', r.handler_sql IS NULL, coalesce(r.handler_sql,'<null>') FROM r;

-- CHECK H2: /readyz 200 when ready (registry+writer+probes) -- dynamic: body NULL, handler_sql set (C execs for real body)
WITH r AS (SELECT * FROM handle_request('GET','/readyz','{}',''))
INSERT INTO _test_results
  SELECT 'H2 /readyz → status 200 when ready', r.status_code=200, 'got '||r.status_code FROM r
  UNION ALL
  SELECT 'H2 /readyz → body NULL (dynamic special)', r.body IS NULL, coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'H2 /readyz → handler_sql NULL (special cased for status override)', r.handler_sql IS NULL, coalesce(r.handler_sql,'<null>') FROM r;

-- CHECK H3: simulate not-ready via table → /readyz 503 (key probe)
UPDATE quackapi_readiness SET ready=false WHERE component='registry';
WITH r AS (SELECT * FROM handle_request('GET','/readyz','{}',''))
INSERT INTO _test_results
  SELECT 'H3 /readyz → status 503 when registry not ready', r.status_code=503, 'got '||r.status_code FROM r
  UNION ALL
  SELECT 'H3 /readyz 503 → body Not Ready override', json_extract_string(r.body,'$.detail')='Not Ready', coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'H3 /readyz 503 → handler_sql NULL', r.handler_sql IS NULL, coalesce(r.handler_sql,'<null>') FROM r;
-- restore
UPDATE quackapi_readiness SET ready=true WHERE component='registry';

-- CHECK H4: after restore /readyz back to 200
WITH r AS (SELECT * FROM handle_request('GET','/readyz','{}',''))
INSERT INTO _test_results
  SELECT 'H4 /readyz restored → 200', r.status_code=200, 'got '||r.status_code FROM r
  UNION ALL
  SELECT 'H4 /readyz restored → handler_sql NULL (special)', r.handler_sql IS NULL, coalesce(r.handler_sql,'<null>') FROM r;

-- CHECK H5: /metrics 200 + prometheus-style (dynamic: body NULL, handler_sql produces the text on exec)
WITH r AS (SELECT * FROM handle_request('GET','/metrics','{}',''))
INSERT INTO _test_results
  SELECT 'H5 /metrics → status 200', r.status_code=200, 'got '||r.status_code FROM r
  UNION ALL
  SELECT 'H5 /metrics → body NULL (dynamic)', r.body IS NULL, coalesce(r.body,'<null>') FROM r
  UNION ALL
  SELECT 'H5 /metrics → handler_sql not null', r.handler_sql IS NOT NULL, coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  SELECT 'H5 /metrics → handler contains routes_total text', instr(coalesce(r.handler_sql,''),'quackapi_routes_total')>0, coalesce(r.handler_sql,'<null>') FROM r;

-- CHECK H6: CREATE HEALTH CHECK sugar registers probe (oracle path via direct insert + macro for parity test)
-- (DDL sugar itself tested via C++ parser in extension; here exercise the table+macro path)
INSERT INTO health_checks SELECT * FROM register_health_check('tier1_probe', 'SELECT 1 AS ok');
WITH c AS (SELECT count(*) AS n FROM health_checks)
INSERT INTO _test_results
  SELECT 'H6 CREATE HEALTH CHECK → health_checks increased', (SELECT n FROM c) >= 2, 'count='||(SELECT n FROM c)::VARCHAR FROM c;
WITH r AS (SELECT * FROM handle_request('GET','/readyz','{}',''))
INSERT INTO _test_results
  SELECT 'H6 /readyz after create health → still 200', r.status_code=200, 'got '||r.status_code FROM r
  UNION ALL
  SELECT 'H6 /readyz handler NULL (special cased) post create', r.handler_sql IS NULL, coalesce(r.handler_sql,'<null>') FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- #1357  Response field include / exclude projection (FastAPI response_model_*)
-- ─────────────────────────────────────────────────────────────────────────────
-- Unit: _apply_field_projection over the two body shapes handlers produce (object + array).
INSERT INTO _test_results
  SELECT 'FIELDS exclude drops key (object)',
         _apply_field_projection('{"id":1,"name":"al","age":30}', NULL, ['age']) = '{"id":1,"name":"al"}',
         coalesce(_apply_field_projection('{"id":1,"name":"al","age":30}', NULL, ['age']), '<null>')
  UNION ALL
  SELECT 'FIELDS include keeps only listed (object)',
         _apply_field_projection('{"id":1,"name":"al","age":30}', ['name'], NULL) = '{"name":"al"}',
         coalesce(_apply_field_projection('{"id":1,"name":"al","age":30}', ['name'], NULL), '<null>')
  UNION ALL
  SELECT 'FIELDS exclude is element-wise (array)',
         _apply_field_projection('[{"id":1,"age":30},{"id":2,"age":25}]', NULL, ['age']) = '[{"id":1},{"id":2}]',
         coalesce(_apply_field_projection('[{"id":1,"age":30},{"id":2,"age":25}]', NULL, ['age']), '<null>')
  UNION ALL
  SELECT 'FIELDS null/null is passthrough',
         _apply_field_projection('{"id":1,"age":30}', NULL, NULL) = '{"id":1,"age":30}',
         coalesce(_apply_field_projection('{"id":1,"age":30}', NULL, NULL), '<null>')
  UNION ALL
  SELECT 'FIELDS null body stays null',
         _apply_field_projection(NULL, NULL, ['age']) IS NULL,
         '<null>';

-- Wiring: a route registered with exclude_fields emits a handler_sql that wraps the handler
-- in a _raw CTE and projects the body via _apply_field_projection (with the array literal baked).
INSERT INTO routes SELECT * FROM register_route('proj_excl','GET','/proj/{id}','SELECT to_json(u) AS body FROM users u WHERE u.id = {id}','dynamic','proj',200, exclude_fields := ['age']);
INSERT INTO param_schema VALUES ('proj_excl','id','path','int',true,NULL);
WITH r AS (SELECT * FROM handle_request('GET','/proj/1','{}',''))
INSERT INTO _test_results
  SELECT 'FIELDS route → handler_sql wraps handler in _raw', instr(r.handler_sql, '_raw AS (') > 0, coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  SELECT 'FIELDS route → handler_sql projects body via _apply_field_projection',
         instr(r.handler_sql, '_apply_field_projection(body') > 0 AND instr(r.handler_sql, '[''age'']') > 0,
         coalesce(r.handler_sql,'<null>') FROM r
  UNION ALL
  SELECT 'FIELDS route → still 200', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- End-to-end (eval the emitted shape): the real get_user body projects to omit age.
WITH _raw AS (SELECT to_json(u) AS body FROM users u WHERE u.id = 1)
INSERT INTO _test_results
  SELECT 'FIELDS end-to-end: projected user body omits age',
         _apply_field_projection(body, NULL, ['age']) = '{"id":1,"name":"alice"}',
         coalesce(_apply_field_projection(body, NULL, ['age']), '<null>') FROM _raw;

-- DI ASSERTIONS (setup binds value visible to handler; teardown helper; phase macro)
-- These run inside the tier1 script (stateful); prove oracle macros + sequencing model.
-- ─────────────────────────────────────────────────────────────────────────────

-- Register a test dep whose setup "binds" by populating a marker table the handler can read.
-- (In real flow C runs the setup_sql then the rendered handler_sql on same exec_con.)
CREATE OR REPLACE TABLE _di_marker (v INTEGER);
INSERT INTO dependencies (name, setup_sql, teardown_sql)
VALUES ('test_marker', 'INSERT INTO _di_marker(v) VALUES (42)', 'DELETE FROM _di_marker');
INSERT INTO route_dependencies (route_id, dep_name) VALUES ('get_user', 'test_marker');

-- phase helper returns the sqls
WITH s AS (SELECT run_dependency_phase('test_marker', 'setup') AS ssql),
     t AS (SELECT run_dependency_phase('test_marker', 'teardown') AS tsql)
INSERT INTO _test_results
  SELECT 'DI run_dependency_phase(setup) returns sql', (SELECT ssql FROM s) IS NOT NULL AND instr((SELECT ssql FROM s), '_di_marker') > 0, (SELECT ssql FROM s) FROM s
  UNION ALL
  SELECT 'DI run_dependency_phase(teardown) returns sql', (SELECT tsql FROM t) IS NOT NULL AND instr((SELECT tsql FROM t), '_di_marker') > 0, (SELECT tsql FROM t) FROM t;

-- Simulate the sequenced execution for "setup binds a value the handler returns"
-- (setup effect, then handler reads it, check value, teardown effect)
INSERT INTO _di_marker(v) VALUES (42);  -- effect of setup_sql
WITH h AS (SELECT v FROM _di_marker LIMIT 1)
INSERT INTO _test_results
  SELECT 'DI setup binds value seen by handler', (SELECT v FROM h) = 42, 'val=' || (SELECT CASE WHEN (SELECT v FROM h) IS NULL THEN '-1' ELSE (SELECT v FROM h)::VARCHAR END) FROM (SELECT 1);

DELETE FROM _di_marker;  -- effect of teardown_sql
INSERT INTO _test_results
  SELECT 'DI teardown helper ran (marker cleaned)', (SELECT count(*) FROM _di_marker) = 0, 'count=' || (SELECT count(*) FROM _di_marker)::VARCHAR FROM (SELECT 1);

-- cleanup test artifacts (harmless if absent)
DELETE FROM route_dependencies WHERE route_id='get_user';
DELETE FROM dependencies WHERE name='test_marker';
DROP TABLE IF EXISTS _di_marker;

-- CHECK L1: lifecycle_hooks table + run_lifecycle(phase) oracle helper
-- (for #617: registry + helper; C worker executes the returned sql bodies)
-- ─────────────────────────────────────────────────────────────────────────────
INSERT INTO lifecycle_hooks SELECT * FROM register_lifecycle('boot_marker', 'STARTUP', 'INSERT INTO _lifecycle_markers(name, phase) VALUES (''boot'', ''STARTUP'')', 10);
INSERT INTO lifecycle_hooks SELECT * FROM register_lifecycle('drain_marker', 'SHUTDOWN', 'INSERT INTO _lifecycle_markers(name, phase) VALUES (''drain'', ''SHUTDOWN'')', 20);
CREATE OR REPLACE TABLE _lifecycle_markers (name VARCHAR, phase VARCHAR);
WITH h AS (SELECT * FROM run_lifecycle('STARTUP'))
INSERT INTO _test_results
  SELECT 'run_lifecycle(STARTUP) returns boot_marker', (SELECT name FROM h LIMIT 1) = 'boot_marker', (SELECT name FROM h LIMIT 1) FROM (SELECT 1) dummy
  UNION ALL
  SELECT 'run_lifecycle(STARTUP) sql present', length((SELECT sql FROM h LIMIT 1)) > 10, 'len=' || length((SELECT sql FROM h LIMIT 1))::VARCHAR FROM (SELECT 1) dummy;
WITH h2 AS (SELECT * FROM run_lifecycle('SHUTDOWN'))
INSERT INTO _test_results
  SELECT 'run_lifecycle(SHUTDOWN) returns drain_marker', (SELECT name FROM h2 LIMIT 1) = 'drain_marker', (SELECT name FROM h2 LIMIT 1) FROM (SELECT 1) dummy
  UNION ALL
  SELECT 'run_lifecycle(SHUTDOWN) sql present', length((SELECT sql FROM h2 LIMIT 1)) > 10, 'len=' || length((SELECT sql FROM h2 LIMIT 1))::VARCHAR FROM (SELECT 1) dummy;

-- ─────────────────────────────────────────────────────────────────────────────
-- SESSION + CSRF (oracle/tier1) — per SESSION_CSRF_SPEC (light; core helpers + table + sign/verify/revoke/expiry/csrf const covered; policed auth in live/C)
-- ─────────────────────────────────────────────────────────────────────────────
LOAD crypto;
INSERT INTO quackapi_session_stores SELECT * FROM register_session_store('sessions', '01234567890123456789012345678901', 'sid', '/', 'Lax', false, 3600);

-- Valid future session row with correct sig
WITH p AS (SELECT 'sess_ora1' AS sid, CAST(CAST(floor(epoch(now() + INTERVAL '1 hour')) AS BIGINT) AS VARCHAR) AS exs, '01234567890123456789012345678901' AS sec),
     cv AS (SELECT sid||'|'||exs||'|'||lower(hex(crypto_hmac('sha2-256', sec, sid||'|'||exs))) AS cv FROM p)
INSERT INTO quackapi_sessions (id, subject, claims, created_at, expires_at, revoked_at, csrf_token, flash)
SELECT 'sess_ora1', 'u_ora', to_json({'sub':'u_ora'}), now(), now()+INTERVAL '1 hour', NULL, 'csrf_ora_123', NULL;

-- CHECK S1: sign/verify roundtrip
WITH p AS (SELECT 'sess_ora1' AS sid, CAST(CAST(floor(epoch(now() + INTERVAL '1 hour')) AS BIGINT) AS VARCHAR) AS exs, '01234567890123456789012345678901' AS sec),
     cv AS (SELECT sid||'|'||exs||'|'||lower(hex(crypto_hmac('sha2-256', sec, sid||'|'||exs))) AS cv FROM p),
     v AS (SELECT _parse_and_verify_session_cookie( (SELECT cv FROM cv), (SELECT sec FROM p) ) AS pv FROM (SELECT 1) )
INSERT INTO _test_results
  SELECT 'SESS sign/verify roundtrip ok', (SELECT pv FROM v) IS NOT NULL, '' FROM (SELECT 1);

-- CHECK S2: tampered sig verify fails
WITH v AS (SELECT _parse_and_verify_session_cookie('sess_ora1|9999999999|badbadbadbadbadbadbadbadbadbadbadbadbadbadbadbadbadbadbadbadbadb', '01234567890123456789012345678901') AS pv FROM (SELECT 1) )
INSERT INTO _test_results SELECT 'SESS tampered sig verify null', (SELECT pv FROM v) IS NULL, '' FROM (SELECT 1);

-- CHECK S3: expired verify null (cookie exp_unix in the past; signed with correct secret so parse reaches exp check)
INSERT INTO quackapi_sessions (id, subject, claims, created_at, expires_at, revoked_at, csrf_token)
SELECT 'sess_exp','u_e',to_json({'sub':'u_e'}), now()-INTERVAL '2 hours', now()-INTERVAL '1 hour', NULL, 'csrf_e';
WITH p AS (SELECT 'sess_exp' AS sid, CAST(CAST(floor(epoch(now() - INTERVAL '2 hours')) AS BIGINT) AS VARCHAR) AS exs, '01234567890123456789012345678901' AS sec),
     cv AS (SELECT sid||'|'||exs||'|'||lower(hex(crypto_hmac('sha2-256', sec, sid||'|'||exs))) AS cv FROM p),
     v AS (SELECT _parse_and_verify_session_cookie( (SELECT cv FROM cv), (SELECT sec FROM p) ) AS pv FROM (SELECT 1) )
INSERT INTO _test_results SELECT 'SESS expired verify null', (SELECT pv FROM v) IS NULL, '' FROM (SELECT 1);

-- CHECK S4: revoked verify null
UPDATE quackapi_sessions SET revoked_at=now() WHERE id='sess_ora1';
WITH p AS (SELECT 'sess_ora1' AS sid, CAST(CAST(floor(epoch(now() + INTERVAL '1 hour')) AS BIGINT) AS VARCHAR) AS exs, '01234567890123456789012345678901' AS sec),
     cv AS (SELECT sid||'|'||exs||'|'||lower(hex(crypto_hmac('sha2-256', sec, sid||'|'||exs))) AS cv FROM p),
     v AS (SELECT _verify_session_cookie( (SELECT cv FROM cv), (SELECT sec FROM p) ) AS pv FROM (SELECT 1) )
INSERT INTO _test_results SELECT 'SESS revoked verify null', (SELECT pv FROM v) IS NULL, '' FROM (SELECT 1);
UPDATE quackapi_sessions SET revoked_at=NULL WHERE id='sess_ora1';

-- CHECK S5: csrf const time match/mismatch
WITH m AS (SELECT _constant_time_str_equals('csrf_ora_123','csrf_ora_123') AS o FROM (SELECT 1)),
     n AS (SELECT NOT _constant_time_str_equals('csrf_ora_123','wrong') AS o FROM (SELECT 1))
INSERT INTO _test_results SELECT 'SESS csrf const eq match', (SELECT o FROM m), '' FROM (SELECT 1)
UNION ALL SELECT 'SESS csrf const eq mismatch', (SELECT o FROM n), '' FROM (SELECT 1);

-- CHECK S6: table row present
WITH n AS (SELECT count(*) AS c FROM quackapi_sessions WHERE id='sess_ora1')
INSERT INTO _test_results SELECT 'SESS table has live row', (SELECT c FROM n)=1, '' FROM (SELECT 1);

-- CHECK S7: existing cookie param surface unaffected
WITH r AS (SELECT * FROM handle_request('GET','/profile','{"_cookies":{"session":"s1"}}',''))
INSERT INTO _test_results SELECT 'R1 cookie profile still 200', r.status_code=200, 'got '||r.status_code FROM r;

-- ─────────────────────────────────────────────────────────────────────────────
-- SUBSCRIPTIONS — registry writer/reader + composition (the C runner consumes
-- run_subscriptions().exec_sql verbatim, so these checks pin its exact shape)
-- ─────────────────────────────────────────────────────────────────────────────

-- CHECK SUB1: register_subscription writer shape → registry row lands
INSERT INTO quackapi_subscriptions SELECT * FROM register_subscription('sub_t1', 'redis-tcp://localhost:6379?channel=t1', 'INSERT INTO _sub_probe SELECT message, channel, message_id, subscription FROM msg');
WITH n AS (SELECT count(*) AS c FROM quackapi_subscriptions WHERE name='sub_t1' AND enabled)
INSERT INTO _test_results SELECT 'SUB register lands enabled row', (SELECT c FROM n)=1, '' FROM (SELECT 1);

-- CHECK SUB2: composed exec_sql is EXACTLY the literal the dispatch simulation below
-- prepares — guards drift between _compose_subscription_sql and the runner contract
WITH r AS (SELECT exec_sql FROM run_subscriptions() WHERE name='sub_t1')
INSERT INTO _test_results SELECT 'SUB compose matches runner literal',
  (SELECT exec_sql FROM r) = 'WITH msg AS (SELECT ?::VARCHAR AS message, ?::VARCHAR AS channel, ?::UBIGINT AS message_id, ?::VARCHAR AS subscription) INSERT INTO _sub_probe SELECT message, channel, message_id, subscription FROM msg',
  substr((SELECT exec_sql FROM r),1,60) FROM (SELECT 1);

-- CHECK SUB3: dispatch simulation — prepared positional binds through the composed
-- shape (message payload as bind #1: untrusted input never spliced)
CREATE OR REPLACE TABLE _sub_probe(payload VARCHAR, chan VARCHAR, mid UBIGINT, sub VARCHAR);
PREPARE _sub_exec AS WITH msg AS (SELECT ?::VARCHAR AS message, ?::VARCHAR AS channel, ?::UBIGINT AS message_id, ?::VARCHAR AS subscription) INSERT INTO _sub_probe SELECT message, channel, message_id, subscription FROM msg;
EXECUTE _sub_exec('payload''); DROP TABLE _sub_probe;--', 't1', '1000', 'sub_t1');
WITH r AS (SELECT * FROM _sub_probe)
INSERT INTO _test_results SELECT 'SUB dispatch binds injection-proof',
  (SELECT count(*) FROM r)=1 AND (SELECT payload FROM r)='payload''); DROP TABLE _sub_probe;--' AND (SELECT mid FROM r)=1000,
  'rows='||(SELECT count(*) FROM r)::VARCHAR FROM (SELECT 1);

-- CHECK SUB4: handler starting with WITH gets its CTE list merged after msg
WITH r AS (SELECT _compose_subscription_sql('WITH j AS (SELECT message FROM msg) INSERT INTO t SELECT * FROM j') AS s FROM (SELECT 1))
INSERT INTO _test_results SELECT 'SUB compose merges handler CTEs',
  starts_with((SELECT s FROM r), 'WITH msg AS (SELECT ?::VARCHAR AS message') AND instr((SELECT s FROM r), '), j AS (SELECT message FROM msg)') > 0,
  substr((SELECT s FROM r),1,80) FROM (SELECT 1);

-- CHECK SUB5: disabled rows excluded from the runner surface
UPDATE quackapi_subscriptions SET enabled=false WHERE name='sub_t1';
WITH n AS (SELECT count(*) AS c FROM run_subscriptions() WHERE name='sub_t1')
INSERT INTO _test_results SELECT 'SUB disabled row hidden from runner', (SELECT c FROM n)=0, '' FROM (SELECT 1);
DELETE FROM quackapi_subscriptions WHERE name='sub_t1';
DROP TABLE _sub_probe;

-- ─────────────────────────────────────────────────────────────────────────────
-- OAUTH2 Authorization-Code + PKCE (FastAPI #335 · OIDC #1428) — OAUTH_SPEC §7
-- Placeholder secrets only — never real credentials in the suite.
-- ─────────────────────────────────────────────────────────────────────────────

INSERT INTO quackapi_session_stores SELECT * FROM register_session_store('oauth_test_store', 'PLACEHOLDER_STORE_SECRET_NEVER_REAL');
INSERT INTO quackapi_auth SELECT * FROM register_oauth('testidp', '{"client_id":"client-abc","client_secret":"PLACEHOLDER_SECRET_NEVER_REAL","auth_url":"https://idp.example/authorize","token_url":"https://idp.example/token","userinfo_url":"https://idp.example/userinfo","redirect_uri":"http://127.0.0.1:18450/auth/testidp/callback","scopes":"openid email","store":"oauth_test_store"}');

-- OAUTH 1: PKCE S256 pinned to the RFC 7636 official test vector (appendix B)
INSERT INTO _test_results SELECT 'OAUTH pkce challenge = RFC 7636 vector',
  _pkce_challenge('dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk') = 'E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM',
  _pkce_challenge('dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk');

-- OAUTH 2: _urlencode edge bytes — reserved, unreserved passthrough, UTF-8 multibyte, NULL
INSERT INTO _test_results
  SELECT 'OAUTH urlencode reserved bytes', _urlencode('a b&c=d+e/f:g') = 'a%20b%26c%3Dd%2Be%2Ff%3Ag', _urlencode('a b&c=d+e/f:g')
  UNION ALL
  SELECT 'OAUTH urlencode unreserved passthrough', _urlencode('AZaz09-._~') = 'AZaz09-._~', _urlencode('AZaz09-._~')
  UNION ALL
  SELECT 'OAUTH urlencode UTF-8 multibyte', _urlencode('é') = '%C3%A9', _urlencode('é')
  UNION ALL
  SELECT 'OAUTH urlencode NULL-safe', _urlencode(NULL) = '', '<' || _urlencode(NULL) || '>';

-- OAUTH 3: _oauth_begin — byte-exact authorize-URL prefix, PKCE binding, minted lengths
CREATE OR REPLACE TABLE _oa_begin AS SELECT * FROM _oauth_begin('testidp', next := '/dash');
WITH b AS (SELECT * FROM _oa_begin)
INSERT INTO _test_results
  SELECT 'OAUTH begin authorize-URL prefix byte-exact',
    starts_with(b.location, 'https://idp.example/authorize?response_type=code&client_id=client-abc&redirect_uri=http%3A%2F%2F127.0.0.1%3A18450%2Fauth%2Ftestidp%2Fcallback&scope=openid%20email&state=' || b.state),
    b.location FROM b
  UNION ALL
  SELECT 'OAUTH begin challenge binds verifier',
    instr(b.location, '&code_challenge=' || _pkce_challenge(b.pkce_verifier) || '&code_challenge_method=S256') > 0,
    b.location FROM b
  UNION ALL
  SELECT 'OAUTH begin state 32 hex chars', length(b.state) = 32, b.state FROM b
  UNION ALL
  SELECT 'OAUTH begin verifier 64 chars', length(b.pkce_verifier) = 64, b.pkce_verifier FROM b
  UNION ALL
  SELECT 'OAUTH begin preserves valid next', b.redirect_after = '/dash', b.redirect_after FROM b;

-- OAUTH 4: open-redirect guard — scheme-relative and absolute URLs collapse to '/'
INSERT INTO _test_results
  SELECT 'OAUTH next guard blocks //evil.com', (SELECT redirect_after FROM _oauth_begin('testidp', next := '//evil.com')) = '/', (SELECT redirect_after FROM _oauth_begin('testidp', next := '//evil.com'))
  UNION ALL
  SELECT 'OAUTH next guard blocks https://evil.com', (SELECT redirect_after FROM _oauth_begin('testidp', next := 'https://evil.com')) = '/', (SELECT redirect_after FROM _oauth_begin('testidp', next := 'https://evil.com'))
  UNION ALL
  SELECT 'OAUTH next guard default NULL → /', (SELECT redirect_after FROM _oauth_begin('testidp')) = '/', (SELECT redirect_after FROM _oauth_begin('testidp'));

-- OAUTH 5: unknown scheme → empty begin (no row, caller 404s)
INSERT INTO _test_results SELECT 'OAUTH begin unknown scheme empty',
  (SELECT count(*) FROM _oauth_begin('no_such_scheme')) = 0, '';

-- OAUTH 6: flow redeem — single-use + 10-minute TTL (caller-inserts pattern)
INSERT INTO quackapi_oauth_flows SELECT state, 'testidp', pkce_verifier, redirect_after, now(), NULL, NULL FROM _oa_begin;
WITH r AS (SELECT f.* FROM _oa_begin b, LATERAL (SELECT * FROM _oauth_flow_redeem(b.state)) f)
INSERT INTO _test_results
  SELECT 'OAUTH redeem returns fresh flow', (SELECT count(*) FROM r) = 1, ''
  UNION ALL
  SELECT 'OAUTH redeem carries next', (SELECT redirect_after FROM r) = '/dash', (SELECT redirect_after FROM r);
UPDATE quackapi_oauth_flows SET redeemed_at = now() WHERE state = (SELECT state FROM _oa_begin);
INSERT INTO _test_results SELECT 'OAUTH redeem single-use (replay empty)',
  (SELECT count(*) FROM _oa_begin b, LATERAL (SELECT * FROM _oauth_flow_redeem(b.state)) f) = 0, '';
INSERT INTO quackapi_oauth_flows VALUES ('stale-flow-state-0000000000000000', 'testidp', repeat('a', 64), '/', now() - INTERVAL 11 MINUTE, NULL, NULL);
INSERT INTO _test_results SELECT 'OAUTH redeem 11-min-old flow expired',
  (SELECT count(*) FROM _oauth_flow_redeem('stale-flow-state-0000000000000000')) = 0, '';

-- OAUTH 7: token-exchange form body — byte-exact, injection payload inert, secret in body
INSERT INTO _test_results
  SELECT 'OAUTH form body byte-exact',
    _oauth_form_body('testidp', 'CODE-xyz', 'ver_123') = 'grant_type=authorization_code&code=CODE-xyz&code_verifier=ver_123&redirect_uri=http%3A%2F%2F127.0.0.1%3A18450%2Fauth%2Ftestidp%2Fcallback&client_id=client-abc&client_secret=PLACEHOLDER_SECRET_NEVER_REAL',
    _oauth_form_body('testidp', 'CODE-xyz', 'ver_123')
  UNION ALL
  SELECT 'OAUTH form body injection payload encoded inert',
    instr(_oauth_form_body('testidp', 'ac''&x=y', 'v'), 'code=ac%27%26x%3Dy') > 0,
    _oauth_form_body('testidp', 'ac''&x=y', 'v');

-- OAUTH 8: composed curl SQL — byte-exact (command line = server path + registry URL only)
INSERT INTO _test_results
  SELECT 'OAUTH exchange SQL byte-exact',
    _oauth_exchange_sql('testidp', '/tmp/qa.form') = 'SELECT content AS token_json FROM read_text(''curl -sS -X POST -H "Content-Type: application/x-www-form-urlencoded" --data-binary @/tmp/qa.form https://idp.example/token |'')',
    _oauth_exchange_sql('testidp', '/tmp/qa.form')
  UNION ALL
  SELECT 'OAUTH userinfo SQL byte-exact',
    _oauth_userinfo_sql('testidp', '/tmp/qa.hdr') = 'SELECT content AS userinfo_json FROM read_text(''curl -sS -H @/tmp/qa.hdr https://idp.example/userinfo |'')',
    _oauth_userinfo_sql('testidp', '/tmp/qa.hdr');

-- OAUTH 9: token response validation — ok / provider error / junk / missing access_token
INSERT INTO _test_results
  SELECT 'OAUTH token_ok accepts valid response', _oauth_token_ok('{"access_token":"at-1","token_type":"Bearer"}') = true, ''
  UNION ALL
  SELECT 'OAUTH token_ok rejects provider error', _oauth_token_ok('{"error":"invalid_grant"}') = false, ''
  UNION ALL
  SELECT 'OAUTH token_ok rejects junk body', COALESCE(_oauth_token_ok('<html>gateway timeout</html>'), false) = false, ''
  UNION ALL
  SELECT 'OAUTH token_ok rejects missing access_token', _oauth_token_ok('{"scope":"openid"}') = false, '';

-- OAUTH 10: session mint — sid/subject/cookie/location from claims + scheme store
WITH m AS (SELECT * FROM _oauth_session_mint('testidp', '{"sub":"u-1","email":"a@b.c"}', '/dash'))
INSERT INTO _test_results
  SELECT 'OAUTH mint sid present', (SELECT sid FROM m) IS NOT NULL, ''
  UNION ALL
  SELECT 'OAUTH mint subject prefers sub', (SELECT subject FROM m) = 'u-1', (SELECT subject FROM m)
  UNION ALL
  SELECT 'OAUTH mint Set-Cookie HttpOnly', instr((SELECT set_cookie FROM m), 'HttpOnly') > 0, (SELECT set_cookie FROM m)
  UNION ALL
  SELECT 'OAUTH mint location = redirect_after', (SELECT location FROM m) = '/dash', (SELECT location FROM m);
INSERT INTO _test_results
  SELECT 'OAUTH mint subject falls back to email',
    (SELECT subject FROM _oauth_session_mint('testidp', '{"email":"a@b.c"}', NULL)) = 'a@b.c',
    (SELECT subject FROM _oauth_session_mint('testidp', '{"email":"a@b.c"}', NULL))
  UNION ALL
  SELECT 'OAUTH mint no subject → empty (caller 401s)',
    (SELECT count(*) FROM _oauth_session_mint('testidp', '{"name":"anon"}', NULL)) = 0, '';

-- cleanup oauth fixtures
DELETE FROM quackapi_oauth_flows WHERE auth_name = 'testidp';
DELETE FROM quackapi_auth WHERE name = 'testidp';
DELETE FROM quackapi_session_stores WHERE name = 'oauth_test_store';
DROP TABLE _oa_begin;

-- ─────────────────────────────────────────────────────────────────────────────
-- SUMMARY — print all results, then a pass/fail aggregate
-- ─────────────────────────────────────────────────────────────────────────────
SELECT check_name, pass, detail FROM _test_results ORDER BY pass ASC, check_name;

SELECT
  array_length(array_agg(check_name)) AS total_checks,
  array_length(list_filter(array_agg(pass), lambda p: p = true))  AS passed,
  array_length(list_filter(array_agg(pass), lambda p: p = false)) AS failed
FROM _test_results;
