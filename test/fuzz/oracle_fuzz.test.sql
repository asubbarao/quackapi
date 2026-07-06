-- ============================================================================
-- Tier-1 property/boundary/fuzz tests for handle_request()
-- Runs AGAINST THE ORACLE — no HTTP server, no build.
--
-- Canonical invocation (self-contained):
--   printf '.read framework.sql\n.read app.sql\n.read test/fuzz/oracle_fuzz.test.sql\n' \
--     | /opt/homebrew/bin/duckdb -unsigned
--
-- app.sql must load before this file (it registers all routes + param_schema).
-- Framework.sql must load before app.sql (defines handle_request + tables).
--
-- Design:
--   Each check is a CTE → INSERT INTO _fuzz_results(check_name, pass, detail).
--   FAIL = a row whose pass=false.
--   Summary row at end counts totals.
--   A bug is a PASS=false row. Every genuine bug is preserved; no test weakened
--   to hide a failure.
-- ============================================================================

CREATE OR REPLACE TABLE _fuzz_results (check_name VARCHAR, pass BOOLEAN, detail VARCHAR);

-- ============================================================================
-- SECTION 1: ROUTER AMBIGUITY / PRECEDENCE
-- ============================================================================

-- 1.1 Empty path → 404 (no route registered for '')
WITH r AS (SELECT * FROM handle_request('GET','','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER empty path → 404', r.status_code = 404, 'got '||r.status_code::VARCHAR FROM r;

-- 1.2 Root path "/" → 404 (no route registered for /)
WITH r AS (SELECT * FROM handle_request('GET','/','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER root slash → 404', r.status_code = 404, 'got '||r.status_code::VARCHAR FROM r;

-- 1.3 Double leading slash "//users" - strip logic produces ['','users'] after split on /
--     list_filter removes empty strings → same as /users
WITH r AS (SELECT * FROM handle_request('GET','//users','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER double slash //users → 200 (same as /users)', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 1.4 Trailing slash "/users/" — segments same as /users after empty-string filter
WITH r AS (SELECT * FROM handle_request('GET','/users/','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER trailing slash /users/ → 200', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 1.5 Trailing slash on param route "/users/1/" — segments same as /users/1
WITH r AS (SELECT * FROM handle_request('GET','/users/1/','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER trailing slash /users/1/ → 200', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 1.6 Deeply nested non-existent path (10 segments) → 404
WITH r AS (SELECT * FROM handle_request('GET','/a/b/c/d/e/f/g/h/i/j','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER deep 10-seg non-existent → 404', r.status_code = 404, 'got '||r.status_code::VARCHAR FROM r;

-- 1.7 Segment count mismatch: /users/1/posts (3 segs, no 3-seg GET route in app.sql) → 404
WITH r AS (SELECT * FROM handle_request('GET','/users/1/posts','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER 3-seg /users/1/posts → 404 (count mismatch)', r.status_code = 404, 'got '||r.status_code::VARCHAR FROM r;

-- 1.8 Percent-encoded segment %2F (literal slash) in path param — treated as literal token
--     /users/%2F → id param = "%2F", try_cast('%2F' AS BIGINT) = NULL → 422
WITH r AS (SELECT * FROM handle_request('GET','/users/%2F','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER pct-encoded slash in id → 422', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 1.9 Unicode segment: /users/αβγ → try_cast → NULL → 422 int_parsing
WITH r AS (SELECT * FROM handle_request('GET','/users/αβγ','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER unicode segment → 422 int_parsing', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 1.10 Very long path (40 identical segments of 10 chars each) → 404 (no matching route)
WITH r AS (SELECT * FROM handle_request('GET','/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcdefghij','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER very long 20-seg path → 404', r.status_code = 404, 'got '||r.status_code::VARCHAR FROM r;

-- 1.11 Literal-segment wins over param-segment (most-literal-wins):
--      Register /users/me and /users/{id}; GET /users/me should pick /users/me
--      app.sql has /users/{id} (get_user). Add /users/me here.
INSERT INTO routes SELECT * FROM register_route('me_route','GET','/users/me','SELECT ''me'' AS body','dynamic','literal me',200);
WITH r AS (SELECT * FROM handle_request('GET','/users/me','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER literal /users/me beats /users/{id}', r.status_code = 200 AND instr(coalesce(r.handler_sql,''), 'me')>0, 'got status='||r.status_code::VARCHAR||' hsql='||coalesce(r.handler_sql,'<null>') FROM r;

-- 1.12 /users/1 still routes to get_user after adding /users/me (no regression)
WITH r AS (SELECT * FROM handle_request('GET','/users/1','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER /users/1 still → get_user after /users/me added', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 1.13 Path with only query string (no path): "?q=hi" → clean_path='', req_segs=[] → 404
WITH r AS (SELECT * FROM handle_request('GET','?q=hi','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER bare query string → 404', r.status_code = 404, 'got '||r.status_code::VARCHAR FROM r;

-- 1.14 Method case: lowercase method 'get' on /health
--      Routes store method as 'GET'; path_matches checks segment structure ignoring method.
--      /health path exists → 405 (path found, method not found) NOT 404.
--      This is correct HTTP semantics: 405 > 404 when path exists.
WITH r AS (SELECT * FROM handle_request('get','/health','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER lowercase method get → 405 (path exists, method mismatch)', r.status_code = 405, 'got '||r.status_code::VARCHAR FROM r;

-- 1.15 Unknown method PATCH on /users → 405 (path exists, method not)
WITH r AS (SELECT * FROM handle_request('PATCH','/users','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER PATCH /users → 405', r.status_code = 405, 'got '||r.status_code::VARCHAR FROM r;

-- 1.16 Multiple ? in path — string_split on ? keeps first segment as clean_path
--      /search?q=a?b → clean_path=/search, query_str='q=a?b' → q value = 'a?b'
--      Should route to search and pass (q required, present)
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a?b','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER multiple ? in URL → 200 (first ? splits path/query)', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 1.17 Query string with no value part: /search?q= → q = empty string (not NULL → not missing)
--      Empty string is a valid string param (required check passes since val_str IS NOT NULL from qmap[key])
WITH r AS (SELECT * FROM handle_request('GET','/search?q=','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER empty string q → 200 (present, not null, empty val)', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- ============================================================================
-- SECTION 2: VALIDATION BOUNDARIES — integer constraints (limit, id)
-- ============================================================================
-- app.sql: limit has constraint {"le":100}, id has type=int (no ge/le constraints)

-- 2.1 limit = 100 (exactly at le bound) → valid → 200
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=100','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND limit=100 (at le) → 200', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 2.2 limit = 101 (one above le=100) → 422 less_than_equal
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=101','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND limit=101 (above le) → 422 less_than_equal', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=101','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND limit=101 → err is less_than_equal',
         json_extract_string(json_extract(r.body,'$.detail[0]'),'$.type') = 'less_than_equal',
         coalesce(json_extract_string(json_extract(r.body,'$.detail[0]'),'$.type'),'<null>') FROM r;

-- 2.3 limit = 99 (one below le=100) → valid → 200
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=99','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND limit=99 (below le) → 200', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 2.4 limit = 0 → valid (no ge constraint on limit) → 200
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=0','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND limit=0 → 200 (no ge constraint)', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 2.5 limit = -1 → valid (no ge constraint) → 200
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=-1','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND limit=-1 → 200 (negative, no ge constraint)', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 2.6 id (path int, no constraint) = 0 → valid → 200
WITH r AS (SELECT * FROM handle_request('GET','/users/0','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND id=0 → 200 (no constraint, valid int)', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 2.7 id = -1 (negative) → valid int → 200
WITH r AS (SELECT * FROM handle_request('GET','/users/-1','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND id=-1 → 200 (negative int, no constraint)', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 2.8 id = +1 (leading +) — try_cast('+1' AS BIGINT) test
WITH r AS (SELECT * FROM handle_request('GET','/users/+1','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND id=+1 → try_cast result (200 or 422)', r.status_code IN (200, 422), 'got '||r.status_code::VARCHAR FROM r;

-- 2.9 id = "99999999999999999999" (overflows BIGINT but fits HUGEINT).
--      The strict-int gate parses via HUGEINT (128-bit), so this now matches
--      FastAPI's unbounded Python int: a valid integer with no such user → the
--      handler runs and returns null with 200 (not a parse error).
WITH r AS (SELECT * FROM handle_request('GET','/users/99999999999999999999','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND id overflow bigint → 200 (HUGEINT holds it, matches FastAPI)', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 2.10 id = "1.5" (float string where int expected).
--      DuckDB's own try_cast('1.5' AS INT) LENIENTLY rounds to 2, but the
--      strict-int gate rejects any value carrying a decimal point. → 422
--      int_parsing, matching FastAPI/Pydantic. (Regression guard for the
--      lenient-cast bug fixed in framework.sql validation_errors.)
WITH r AS (SELECT * FROM handle_request('GET','/users/1.5','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND id=1.5 → 422 int_parsing (strict gate rejects decimal, matches FastAPI)', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 2.11 id = "1e3" (scientific notation).
--      DuckDB try_cast('1e3' AS INT) leniently parses 1000; the strict-int gate
--      rejects any value carrying an exponent marker. → 422, matching FastAPI.
WITH r AS (SELECT * FROM handle_request('GET','/users/1e3','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND id=1e3 → 422 int_parsing (strict gate rejects exponent, matches FastAPI)', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 2.12 id = "0x10" (hex). Not a valid HUGEINT literal → int_parsing 422,
--      matching FastAPI (which rejects "0x10" as a non-integer string).
WITH r AS (SELECT * FROM handle_request('GET','/users/0x10','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND id=0x10 → 422 int_parsing (non-integer literal, matches FastAPI)', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 2.13 limit = "  5  " (whitespace-padded) → try_cast(' 5 ' AS BIGINT) test
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit= 5 ','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND limit whitespace-padded → 200 or 422 (try_cast behavior)', r.status_code IN (200, 422), 'got '||r.status_code::VARCHAR FROM r;

-- 2.14 limit = "null" (literal string null, not JSON null) → try_cast('null' AS BIGINT) = NULL → 422
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=null','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND limit="null" string → 422 int_parsing', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 2.15 limit with leading zero: "007" — try_cast('007' AS BIGINT) = 7 → valid → 200
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=007','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND limit=007 (leading zeros) → 200 (valid int)', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 2.15b STRICT-INT GATE regression battery (framework.sql validation_errors).
--       DuckDB's native try_cast is lenient (rounds '1.5'→2, parses '1e2'→100,
--       parses hex); FastAPI/Pydantic rejects all non-integer strings. The gate
--       must reject decimal/exponent/hex/garbage while accepting pure integers
--       (incl. sign, leading zeros, surrounding whitespace, and HUGEINT-range).
--       Negative cases → 422 int_parsing:
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=1.0','{}',''))
INSERT INTO _fuzz_results
  SELECT 'STRICT limit=1.0 (trailing .0) → 422', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=1E2','{}',''))
INSERT INTO _fuzz_results
  SELECT 'STRICT limit=1E2 (uppercase exponent) → 422', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=.5','{}',''))
INSERT INTO _fuzz_results
  SELECT 'STRICT limit=.5 → 422', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=Infinity','{}',''))
INSERT INTO _fuzz_results
  SELECT 'STRICT limit=Infinity → 422', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;
--       Positive cases (valid ints) must NOT be flagged by the gate:
WITH r AS (SELECT * FROM handle_request('GET','/users/-5','{}',''))
INSERT INTO _fuzz_results
  SELECT 'STRICT id=-5 (negative) → not int_parsing (200)', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=+7','{}',''))
INSERT INTO _fuzz_results
  SELECT 'STRICT limit=+7 (leading plus) → 200', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 2.16 Register a route with a ge constraint to test lower bound
INSERT INTO routes SELECT * FROM register_route('ge_test','GET','/range/{val}','SELECT {val} AS body','dynamic','ge test',200);
INSERT INTO param_schema (route_id, name, location, type, required, constraint_json)
VALUES ('ge_test', 'val', 'path', 'int', true, '{"ge":10}');

-- val=10 (at ge) → 200
WITH r AS (SELECT * FROM handle_request('GET','/range/10','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND ge_test val=10 (at ge) → 200', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- val=9 (one below ge=10) → 422 greater_than_equal
WITH r AS (SELECT * FROM handle_request('GET','/range/9','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND ge_test val=9 (below ge) → 422 greater_than_equal', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;
WITH r AS (SELECT * FROM handle_request('GET','/range/9','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND ge_test → err is greater_than_equal',
         json_extract_string(json_extract(r.body,'$.detail[0]'),'$.type') = 'greater_than_equal',
         coalesce(json_extract_string(json_extract(r.body,'$.detail[0]'),'$.type'),'<null>') FROM r;

-- val=11 (above ge=10) → 200
WITH r AS (SELECT * FROM handle_request('GET','/range/11','{}',''))
INSERT INTO _fuzz_results
  SELECT 'BOUND ge_test val=11 (above ge) → 200', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- ============================================================================
-- SECTION 3: BODY EDGE CASES (POST /users: name str req, age int req)
-- ============================================================================

-- 3.1 Malformed JSON body → FIXED: try() around body json_extract_string means a
--     non-JSON body no longer throws; body params resolve to NULL → 422 missing.
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','not-json-at-all'))
INSERT INTO _fuzz_results
  SELECT 'BODY malformed JSON → 422 (was: Invalid Input Error crash)',
         (status_code = 422),
         'status=' || status_code FROM r;

-- 3.2 Empty string body → params missing → 422 (2 errors: name + age missing)
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','')),
     detail AS (SELECT json_extract(r.body,'$.detail')::JSON[] AS arr FROM r)
INSERT INTO _fuzz_results
  SELECT 'BODY empty string → 422', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM handle_request('POST','/users','{}','') r
  UNION ALL
  SELECT 'BODY empty string → 2 errors', array_length((SELECT arr FROM detail)) = 2, (SELECT arr FROM detail)::VARCHAR FROM detail;

-- 3.3 NULL body literal (SQL NULL, not string) → params missing → 422
WITH r AS (SELECT * FROM handle_request('POST','/users','{}',NULL))
INSERT INTO _fuzz_results
  SELECT 'BODY NULL (SQL NULL) → 422', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 3.4 JSON null body (string "null") → json_extract_string('null','$.name') = NULL → both missing → 422
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','null'))
INSERT INTO _fuzz_results
  SELECT 'BODY JSON null string → 422', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 3.5 JSON array body "[]" instead of object → json_extract_string returns NULL → both missing → 422
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','[]'))
INSERT INTO _fuzz_results
  SELECT 'BODY JSON array → 422 (not object)', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 3.6 JSON array body "[1,2,3]" → same: no name/age keys → 422
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','[1,2,3]'))
INSERT INTO _fuzz_results
  SELECT 'BODY JSON array [1,2,3] → 422', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 3.7 Body with extra unknown fields (name+age present, extra keys ignored) → 201
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"extra","age":5,"foo":"bar","baz":99}'))
INSERT INTO _fuzz_results
  SELECT 'BODY extra fields → 201 (ignored)', r.status_code = 201, 'got '||r.status_code::VARCHAR FROM r;

-- 3.8 age as JSON null: {"name":"x","age":null}
--     json_extract_string('{"age":null}','$.age') = NULL in DuckDB (JSON null → SQL NULL)
--     → age is required AND NULL → 422 missing
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"x","age":null}'))
INSERT INTO _fuzz_results
  SELECT 'BODY age JSON null → 422 missing (JSON null = SQL NULL)', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;
-- Verify the error is specifically for age
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"x","age":null}'))
INSERT INTO _fuzz_results
  SELECT 'BODY age JSON null → err field is age',
         json_extract_string(
           list_filter(
             json_extract(r.body,'$.detail')::JSON[],
             lambda e: json_extract_string(e,'$.loc[1]') = 'age'
           )[1],
           '$.type'
         ) = 'missing',
         coalesce(r.body,'<null>') FROM r;

-- 3.9 name as JSON null: {"name":null,"age":5} → name missing → 422
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":null,"age":5}'))
INSERT INTO _fuzz_results
  SELECT 'BODY name JSON null → 422 missing', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 3.10 age as float in JSON: {"name":"x","age":1.5}
--      json_extract_string returns '1.5' (as string). DuckDB's own cast would
--      round '1.5'→2, but the strict-int gate rejects the decimal point → 422
--      int_parsing, matching FastAPI which rejects {"age":1.5} for an int field.
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"x","age":1.5}'))
INSERT INTO _fuzz_results
  SELECT 'BODY age=1.5 (float in JSON) → 422 int_parsing (strict gate, matches FastAPI)', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 3.11 age as boolean in JSON: {"name":"x","age":true} → json_extract_string returns 'true' → try_cast('true' AS BIGINT) = NULL → 422 int_parsing
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"x","age":true}'))
INSERT INTO _fuzz_results
  SELECT 'BODY age=true (bool in JSON) → 422 int_parsing', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 3.12 Body with ONLY whitespace → FIXED: the null-body guard is now
--      `body IS NULL OR trim(body) = ''`, so whitespace-only resolves to 422 missing.
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','   '))
INSERT INTO _fuzz_results
  SELECT 'BODY whitespace-only → 422 (was: crash)',
         (status_code = 422), 'status=' || status_code FROM r;

-- 3.13 Duplicate keys in JSON body: last writer wins in DuckDB → valid if last value is correct type
--      {"name":"x","name":"y","age":5} → json_extract_string keeps one value → valid 201
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"x","name":"y","age":5}'))
INSERT INTO _fuzz_results
  SELECT 'BODY duplicate keys → 201 (one value taken)', r.status_code = 201, 'got '||r.status_code::VARCHAR FROM r;

-- 3.14 Deeply nested body (30 levels) — valid JSON but keys at wrong level → missing → 422
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"data":{"inner":{"deep":{"name":"x","age":5}}}}'))
INSERT INTO _fuzz_results
  SELECT 'BODY keys deeply nested → 422 (top-level keys missing)', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- ============================================================================
-- SECTION 4: 422 SHAPE INVARIANTS
-- ============================================================================

-- 4.1 Every 422 has a "detail" key that is a JSON array (not a string, not null)
--     Test multiple sources of 422: type error, constraint, missing

-- 4.1a int_parsing error: $.detail must be array
WITH r AS (SELECT * FROM handle_request('GET','/users/abc','{}',''))
INSERT INTO _fuzz_results
  SELECT '422 SHAPE int_parsing → detail is array',
         json_type(json_extract(r.body,'$.detail')) = 'ARRAY',
         coalesce(json_type(json_extract(r.body,'$.detail')),'<null>') FROM r;

-- 4.1b missing error: $.detail must be array
WITH r AS (SELECT * FROM handle_request('POST','/users','{}',''))
INSERT INTO _fuzz_results
  SELECT '422 SHAPE missing → detail is array',
         json_type(json_extract(r.body,'$.detail')) = 'ARRAY',
         coalesce(json_type(json_extract(r.body,'$.detail')),'<null>') FROM r;

-- 4.1c constraint error: $.detail must be array
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=999','{}',''))
INSERT INTO _fuzz_results
  SELECT '422 SHAPE constraint → detail is array',
         json_type(json_extract(r.body,'$.detail')) = 'ARRAY',
         coalesce(json_type(json_extract(r.body,'$.detail')),'<null>') FROM r;

-- 4.2 Each entry in detail[] has 'type', 'loc', 'msg' keys
WITH r AS (SELECT * FROM handle_request('GET','/users/abc','{}','')),
     entry AS (SELECT json_extract(r.body,'$.detail[0]') AS e FROM r)
INSERT INTO _fuzz_results
  SELECT '422 SHAPE detail[0] has type key',
         json_extract_string((SELECT e FROM entry), '$.type') IS NOT NULL,
         (SELECT e FROM entry)::VARCHAR
  UNION ALL
  SELECT '422 SHAPE detail[0] has loc key',
         json_type(json_extract((SELECT e FROM entry), '$.loc')) = 'ARRAY',
         (SELECT e FROM entry)::VARCHAR
  UNION ALL
  SELECT '422 SHAPE detail[0] has msg key',
         json_extract_string((SELECT e FROM entry), '$.msg') IS NOT NULL,
         (SELECT e FROM entry)::VARCHAR
  UNION ALL
  SELECT '422 SHAPE detail[0] has input key (PARSE)',
         json_extract((SELECT e FROM entry), '$.input') IS NOT NULL,
         (SELECT e FROM entry)::VARCHAR;

-- 4.3 loc array structure: [location_string, field_name]
--     For path param 'id': loc = ["path","id"]
WITH r AS (SELECT * FROM handle_request('GET','/users/abc','{}','')),
     entry AS (SELECT json_extract(r.body,'$.detail[0]') AS e FROM r)
INSERT INTO _fuzz_results
  SELECT '422 SHAPE loc[0] = path (for path param)',
         json_extract_string((SELECT e FROM entry), '$.loc[0]') = 'path',
         (SELECT e FROM entry)::VARCHAR
  UNION ALL
  SELECT '422 SHAPE loc[1] = id (field name)',
         json_extract_string((SELECT e FROM entry), '$.loc[1]') = 'id',
         (SELECT e FROM entry)::VARCHAR;

-- 4.4 loc for query param: ["query","limit"] on constraint violation
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=999','{}','')),
     entry AS (SELECT json_extract(r.body,'$.detail[0]') AS e FROM r)
INSERT INTO _fuzz_results
  SELECT '422 SHAPE limit constraint → loc[0] = query',
         json_extract_string((SELECT e FROM entry), '$.loc[0]') = 'query',
         (SELECT e FROM entry)::VARCHAR
  UNION ALL
  SELECT '422 SHAPE limit constraint → loc[1] = limit',
         json_extract_string((SELECT e FROM entry), '$.loc[1]') = 'limit',
         (SELECT e FROM entry)::VARCHAR;

-- 4.5 loc for body param: ["body","age"] on missing
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"x"}'))
INSERT INTO _fuzz_results
  SELECT '422 SHAPE missing body param → loc[0] = body',
         json_extract_string(
           list_filter(
             json_extract(r.body,'$.detail')::JSON[],
             lambda e: json_extract_string(e,'$.loc[1]') = 'age'
           )[1],
           '$.loc[0]'
         ) = 'body',
         coalesce(r.body,'<null>') FROM r;

-- 4.6 ALL errors aggregate (not just first):
--     POST /users with empty body → BOTH name AND age must appear in detail[]
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','')),
     detail AS (SELECT json_extract(r.body,'$.detail')::JSON[] AS arr FROM r)
INSERT INTO _fuzz_results
  SELECT '422 AGGREGATE all errors → 2 errors for 2 missing',
         array_length((SELECT arr FROM detail)) = 2,
         (SELECT arr FROM detail)::VARCHAR FROM detail
  UNION ALL
  SELECT '422 AGGREGATE → name error present',
         array_length(list_filter(
           (SELECT arr FROM detail),
           lambda e: json_extract_string(e,'$.loc[1]') = 'name'
         )) > 0,
         (SELECT arr FROM detail)::VARCHAR FROM detail
  UNION ALL
  SELECT '422 AGGREGATE → age error present',
         array_length(list_filter(
           (SELECT arr FROM detail),
           lambda e: json_extract_string(e,'$.loc[1]') = 'age'
         )) > 0,
         (SELECT arr FROM detail)::VARCHAR FROM detail;

-- 4.7 Error message content matches FastAPI phrasing
WITH r AS (SELECT * FROM handle_request('GET','/users/abc','{}',''))
INSERT INTO _fuzz_results
  SELECT '422 MSG int_parsing phrasing',
         starts_with(
           coalesce(json_extract_string(json_extract(r.body,'$.detail[0]'),'$.msg'),''),
           'Input should be a valid integer'
         ),
         coalesce(json_extract_string(json_extract(r.body,'$.detail[0]'),'$.msg'),'<null>') FROM r;

WITH r AS (SELECT * FROM handle_request('POST','/users','{}',''))
INSERT INTO _fuzz_results
  SELECT '422 MSG missing phrasing',
         json_extract_string(
           list_filter(
             json_extract(r.body,'$.detail')::JSON[],
             lambda e: json_extract_string(e,'$.type') = 'missing'
           )[1],
           '$.msg'
         ) = 'Field required',
         coalesce(r.body,'<null>') FROM r;

WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=999','{}',''))
INSERT INTO _fuzz_results
  SELECT '422 MSG less_than_equal mentions bound (100)',
         instr(
           coalesce(json_extract_string(json_extract(r.body,'$.detail[0]'),'$.msg'),''),
           '100'
         ) > 0,
         coalesce(json_extract_string(json_extract(r.body,'$.detail[0]'),'$.msg'),'<null>') FROM r;

-- 4.8 422 handler_sql must be NULL (never leak partial SQL on validation error)
WITH r AS (SELECT * FROM handle_request('GET','/users/abc','{}',''))
INSERT INTO _fuzz_results
  SELECT '422 handler_sql must be NULL', r.handler_sql IS NULL, coalesce(r.handler_sql,'<null>') FROM r;
WITH r AS (SELECT * FROM handle_request('POST','/users','{}',''))
INSERT INTO _fuzz_results
  SELECT '422 handler_sql NULL (missing body)', r.handler_sql IS NULL, coalesce(r.handler_sql,'<null>') FROM r;

-- ============================================================================
-- SECTION 5: IDEMPOTENCE / DETERMINISM
-- ============================================================================

-- 5.1 Same GET /users/5 called twice → identical status_code
WITH a AS (SELECT * FROM handle_request('GET','/users/5','{}','')),
     b AS (SELECT * FROM handle_request('GET','/users/5','{}',''))
INSERT INTO _fuzz_results
  SELECT 'IDEM GET /users/5 status same on repeat',
         (SELECT status_code FROM a) = (SELECT status_code FROM b),
         'a='||(SELECT status_code::VARCHAR FROM a)||' b='||(SELECT status_code::VARCHAR FROM b);

-- 5.2 Same GET /users/5 called twice → identical content_type
WITH a AS (SELECT * FROM handle_request('GET','/users/5','{}','')),
     b AS (SELECT * FROM handle_request('GET','/users/5','{}',''))
INSERT INTO _fuzz_results
  SELECT 'IDEM GET /users/5 content_type same',
         (SELECT content_type FROM a) = (SELECT content_type FROM b),
         'a='||(SELECT content_type FROM a)||' b='||(SELECT content_type FROM b);

-- 5.3 Same GET /users/5 called twice → identical handler_sql
WITH a AS (SELECT * FROM handle_request('GET','/users/5','{}','')),
     b AS (SELECT * FROM handle_request('GET','/users/5','{}',''))
INSERT INTO _fuzz_results
  SELECT 'IDEM GET /users/5 handler_sql same',
         (SELECT handler_sql FROM a) = (SELECT handler_sql FROM b),
         'a='||coalesce((SELECT handler_sql FROM a),'<null>');

-- 5.4 Same 422 query called twice → identical body
WITH a AS (SELECT * FROM handle_request('GET','/users/abc','{}','')),
     b AS (SELECT * FROM handle_request('GET','/users/abc','{}',''))
INSERT INTO _fuzz_results
  SELECT 'IDEM 422 body same on repeat',
         (SELECT body FROM a) = (SELECT body FROM b),
         'a='||coalesce((SELECT body FROM a),'<null>');

-- 5.5 Static route idempotence: GET /health returns same body
WITH a AS (SELECT * FROM handle_request('GET','/health','{}','')),
     b AS (SELECT * FROM handle_request('GET','/health','{}',''))
INSERT INTO _fuzz_results
  SELECT 'IDEM /health body same on repeat',
         (SELECT body FROM a) = (SELECT body FROM b),
         'a='||coalesce((SELECT body FROM a),'<null>');

-- ============================================================================
-- SECTION 6: ADDITIONAL EDGE CASES — method / header / 404 / 405 variants
-- ============================================================================

-- 6.1 PUT /users/1 → 405 (GET and (implicit HEAD) exist, not PUT)
WITH r AS (SELECT * FROM handle_request('PUT','/users/1','{}',''))
INSERT INTO _fuzz_results
  SELECT 'METHOD PUT /users/1 → 405', r.status_code = 405, 'got '||r.status_code::VARCHAR FROM r;

-- 6.2 Allow header on 405 must list GET and HEAD (since GET exists, HEAD auto-added)
WITH r AS (SELECT * FROM handle_request('PUT','/users/1','{}',''))
INSERT INTO _fuzz_results
  SELECT 'METHOD 405 Allow contains GET',
         instr(coalesce(json_extract_string(r.resp_headers,'$.Allow'),''),'GET') > 0,
         r.resp_headers FROM r
  UNION ALL
  SELECT 'METHOD 405 Allow contains HEAD',
         instr(coalesce(json_extract_string(r.resp_headers,'$.Allow'),''),'HEAD') > 0,
         r.resp_headers FROM r;

-- 6.3 HEAD on /search with valid params → same status as GET
WITH hd AS (SELECT * FROM handle_request('HEAD','/search?q=a','{}','')),
     gt AS (SELECT * FROM handle_request('GET', '/search?q=a','{}',''))
INSERT INTO _fuzz_results
  SELECT 'HEAD /search status = GET /search status',
         (SELECT status_code FROM hd) = (SELECT status_code FROM gt),
         'head='||(SELECT status_code::VARCHAR FROM hd)||' get='||(SELECT status_code::VARCHAR FROM gt);

-- 6.4 HEAD on /search with invalid limit → same 422 as GET
WITH hd AS (SELECT * FROM handle_request('HEAD','/search?q=a&limit=abc','{}','')),
     gt AS (SELECT * FROM handle_request('GET', '/search?q=a&limit=abc','{}',''))
INSERT INTO _fuzz_results
  SELECT 'HEAD /search invalid param → same 422 as GET',
         (SELECT status_code FROM hd) = (SELECT status_code FROM gt),
         'head='||(SELECT status_code::VARCHAR FROM hd)||' get='||(SELECT status_code::VARCHAR FROM gt);

-- 6.5 404 response has content_type application/json
WITH r AS (SELECT * FROM handle_request('GET','/no-such-route','{}',''))
INSERT INTO _fuzz_results
  SELECT '404 content_type = application/json', r.content_type = 'application/json', r.content_type FROM r;

-- 6.6 404 response body has {"detail":"Not Found"}
WITH r AS (SELECT * FROM handle_request('GET','/no-such-route','{}',''))
INSERT INTO _fuzz_results
  SELECT '404 body detail = Not Found',
         json_extract_string(r.body,'$.detail') = 'Not Found',
         coalesce(r.body,'<null>') FROM r;

-- 6.7 405 content_type = application/json
WITH r AS (SELECT * FROM handle_request('PUT','/health','{}',''))
INSERT INTO _fuzz_results
  SELECT '405 content_type = application/json', r.content_type = 'application/json', r.content_type FROM r;

-- 6.8 /events → 200, content_type text/event-stream, handler_sql not NULL
WITH r AS (SELECT * FROM handle_request('GET','/events','{}',''))
INSERT INTO _fuzz_results
  SELECT 'STREAM /events → 200',                r.status_code = 200,              'got '||r.status_code::VARCHAR FROM r
  UNION ALL
  SELECT 'STREAM /events → text/event-stream', r.content_type = 'text/event-stream', r.content_type FROM r
  UNION ALL
  SELECT 'STREAM /events → handler_sql set',   r.handler_sql IS NOT NULL,         coalesce(r.handler_sql,'<null>') FROM r;

-- 6.9 /openapi.json → body is valid JSON (has "openapi" key)
WITH r AS (SELECT * FROM handle_request('GET','/openapi.json','{}',''))
INSERT INTO _fuzz_results
  SELECT 'OPENAPI body has openapi key = 3.0.0',
         json_extract_string(r.body,'$.openapi') = '3.0.0',
         coalesce(json_extract_string(r.body,'$.openapi'),'<null>') FROM r;

-- 6.10 query string key with no = sign: /search?q → qmap['q'] = NULL or empty?
--      string_split('q','=') = ['q']; list_element(['q'],1) = 'q'; list_element(['q'],2) = NULL
--      → qmap['q'] = COALESCE(NULL,'') = '' → not null → required check PASSES
--      So q is empty string, not missing. Status 200.
WITH r AS (SELECT * FROM handle_request('GET','/search?q','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER query key without = → 200 (empty val, present)', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 6.11 Query string repeated key: /search?q=a&q=b → FIXED: _qs_to_map dedups keys
--      keeping the LAST occurrence (Starlette scalar semantics), so no crash. 200.
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&q=b','{}',''))
INSERT INTO _fuzz_results
  SELECT 'ROUTER duplicate query key → 200 (was: Map keys must be unique crash)',
         (status_code = 200), 'status=' || status_code FROM r;

-- 6.12 Body for GET request (ignored — GET /health doesn't have body params)
WITH r AS (SELECT * FROM handle_request('GET','/health','{}','{"ignored":"data"}'))
INSERT INTO _fuzz_results
  SELECT 'GET body ignored → 200', r.status_code = 200, 'got '||r.status_code::VARCHAR FROM r;

-- 6.13 SQL injection attempt in path param (raw value goes into SQL literal — type check first)
--      /users/1;DROP TABLE users → try_cast('1;DROP TABLE users' AS BIGINT) = NULL → 422
--      The injection string never reaches handler_sql rendering since it fails type check first
WITH r AS (SELECT * FROM handle_request('GET','/users/1;DROP TABLE users','{}',''))
INSERT INTO _fuzz_results
  SELECT 'SECURITY sql-inject in path param → 422 (blocked by type check)', r.status_code = 422, 'got '||r.status_code::VARCHAR FROM r;

-- 6.14 SQL injection in string body param — name is string type, no type validation.
--      The string is quoted with replace(val,"'","''") so injection is escaped.
--      /users POST with name = "'; DROP TABLE users; --"
--      handler_sql should contain the safely-escaped literal.
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"'' OR 1=1 --","age":5}'))
INSERT INTO _fuzz_results
  SELECT 'SECURITY sql-inject string param → 201 (rendered, escaped in SQL literal)',
         r.status_code = 201,
         'got '||r.status_code::VARCHAR FROM r;
-- Verify handler_sql contains doubled single-quote (escape evidence)
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"'' OR 1=1 --","age":5}'))
INSERT INTO _fuzz_results
  SELECT 'SECURITY escaped name → handler_sql has doubled quote',
         instr(coalesce(r.handler_sql,''), '''''') > 0,
         coalesce(r.handler_sql,'<null>') FROM r;

-- ============================================================================
-- SECTION 7: JSON-NULL VS ABSENT KEY BOUNDARY (JSONB-null trap analog)
-- ============================================================================
-- In JSON: {"age":null} → json_extract_string returns SQL NULL (same as absent key).
-- This is the CLASSIC trap documented in .claude/rules/jsonb-null-handling.md:
-- a field that is JSON null looks exactly like a missing field to the validator.
-- Document observed behavior as ground-truth assertions.

-- 7.1 Missing key vs JSON null for required int → BOTH produce "missing" (same behavior)
WITH absent AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"x"}')),
     jnull  AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"x","age":null}'))
INSERT INTO _fuzz_results
  SELECT 'JSONB-NULL absent key → 422',       (SELECT status_code FROM absent) = 422, 'got '||(SELECT status_code::VARCHAR FROM absent)
  UNION ALL
  SELECT 'JSONB-NULL json null → 422',        (SELECT status_code FROM jnull) = 422,  'got '||(SELECT status_code::VARCHAR FROM jnull)
  UNION ALL
  SELECT 'JSONB-NULL both produce same status', (SELECT status_code FROM absent) = (SELECT status_code FROM jnull),
         'absent='||(SELECT status_code::VARCHAR FROM absent)||' jnull='||(SELECT status_code::VARCHAR FROM jnull);

-- 7.2 Verify error type is 'missing' for JSON null (not 'int_parsing')
--     json_extract_string('{"age":null}','$.age') = NULL → pv.val_str IS NULL → missing check fires
--     NOT int_parsing (int_parsing only fires when val_str IS NOT NULL)
WITH r AS (SELECT * FROM handle_request('POST','/users','{}','{"name":"x","age":null}'))
INSERT INTO _fuzz_results
  SELECT 'JSONB-NULL age json null → err type is missing (not int_parsing)',
         json_extract_string(
           list_filter(
             json_extract(r.body,'$.detail')::JSON[],
             lambda e: json_extract_string(e,'$.loc[1]') = 'age'
           )[1],
           '$.type'
         ) = 'missing',
         coalesce(r.body,'<null>') FROM r;

-- 7.3 Optional int param with JSON null in query → age is in body (skip), but
--     for query: limit is optional. Test query param absent vs explicit empty string.
--     /search?q=a (no limit) → 200
--     /search?q=a&limit= (empty string) → try_cast('', BIGINT) = NULL → 422 int_parsing
--     This demonstrates empty != absent for optional params.
WITH r AS (SELECT * FROM handle_request('GET','/search?q=a&limit=','{}',''))
INSERT INTO _fuzz_results
  SELECT 'JSONB-NULL empty limit string → 422 int_parsing (empty != absent)',
         r.status_code = 422,
         'got '||r.status_code::VARCHAR FROM r;

-- ============================================================================
-- SUMMARY
-- ============================================================================
SELECT check_name, pass, detail FROM _fuzz_results ORDER BY pass ASC, check_name;

SELECT
  array_length(array_agg(check_name)) AS total_checks,
  array_length(list_filter(array_agg(pass), lambda p: p = true))  AS passed,
  array_length(list_filter(array_agg(pass), lambda p: p = false)) AS failed
FROM _fuzz_results;
