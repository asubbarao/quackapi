-- ============================================================================
-- quackapi bootstrap — extensions + HTTP CLIENT POLICY (curl_httpfs is the law)
-- ============================================================================
-- json is a core extension. The stock CLI autoloads it on first use, but a
-- statically-built binary (the compiled quackapi extension's bundled duckdb, and
-- the serve_brain worker connections) may have autoload disabled — without an
-- explicit LOAD the entire route seed (json_each / json_object / JSON type) and
-- every to_json handler silently fails to bind. Load it up front, unconditionally.
INSTALL json; LOAD json;
INSTALL shellfs FROM community; LOAD shellfs;

-- HTTP CLIENT POLICY — curl_httpfs is the DEFAULT, and it is the whole point of
-- running DuckDB as a *server*. Any handler that reads remote data
-- (read_text / read_csv / read_parquet / read_json over http(s)) gets a
-- connection pool + HTTP/2 multiplexing + async IO, so a multi-file fetch of N
-- urls runs CONCURRENTLY. Measured (30 README urls, one session):
--     curl     -> 0.021 s
--     httplib  -> 10.164 s        (~480x)
-- The stock 'httplib' client is serial GET; it makes any HTTP-fanout server look
-- broken. So for quackapi, curl is not a tuning option — it is the default wiring.
--
-- *** ESCAPE HATCH — you have to mean it (this is soldered, not a toggle) ***
-- To fall back to the stock serial client, change 'curl' -> 'httplib' in BOTH
-- soldered sites:
--     1. the SET below (governs the CLI instance: gates, /openapi.json), and
--     2. the IDENTICAL SET inside serve_brain.sql worker_main (the C accept-loop;
--        governs every request-serving worker connection — the one that matters).
-- Flipping only one leaves the server half-curl / half-httplib. The double-wiring
-- is deliberate: opting out of concurrency for a server should take real intent.
INSTALL curl_httpfs FROM community; LOAD curl_httpfs;
INSTALL httpfs_timeout_retry FROM community; LOAD httpfs_timeout_retry;
SET httpfs_client_implementation = 'curl';   -- <<< soldered default. 'httplib' = serial opt-out.
SET http_retries = 3;                        -- transient-failure resilience for a long-lived server
SET httpfs_retries_file_operation = 3;
SET http_timeout = 30000;

-- crypto (for CREATE AUTH JWT HS256 verification via crypto_hmac + base64). Optional for non-auth use.
-- LOAD here so macro bodies referencing crypto_hmac parse successfully even if no prior LOAD.
-- If extension not present, JWT auth paths will fail verification at runtime (expected).
INSTALL crypto FROM community; LOAD crypto;

CREATE SEQUENCE IF NOT EXISTS users_id_seq START 100;
CREATE TABLE IF NOT EXISTS users (id INTEGER DEFAULT nextval('users_id_seq'), name VARCHAR, age INTEGER);
TRUNCATE TABLE users;
INSERT INTO users (id, name, age) VALUES (1,'alice',30), (2,'bob',25), (3,'carol',40);

CREATE OR REPLACE TABLE routes (
  route_id VARCHAR,
  method VARCHAR,
  pattern VARCHAR,
  handler VARCHAR,
  kind VARCHAR,
  summary VARCHAR,
  status INTEGER
);

CREATE OR REPLACE TABLE param_schema (
  route_id VARCHAR,
  name VARCHAR,
  location VARCHAR,
  type VARCHAR,
  required BOOLEAN,
  constraint_json VARCHAR
);

-- route_headers: per-route static response headers (additive to routes).
-- Used for Set-Cookie rows and Location for redirects. Cleaner than widening routes.
-- register_route callers can INSERT INTO route_headers after registering the route.
CREATE OR REPLACE TABLE route_headers (
  route_id VARCHAR,
  name VARCHAR,
  value VARCHAR
);

-- register_route macro: app.sql calls this to produce route rows (no VALUES-literal hardcode of config rows in app).
-- Usage: INSERT INTO routes SELECT * FROM register_route('id', 'GET', '/p', 'HANDLER', 'dynamic', 'sum', 200);
CREATE OR REPLACE MACRO register_route(route_id, method, pattern, handler, kind, summary, status := 200) AS TABLE (
  SELECT
    route_id AS route_id,
    method AS method,
    pattern AS pattern,
    handler AS handler,
    kind AS kind,
    summary AS summary,
    status AS status
);

-- register_redirect: convenience for 3xx redirects. Inserts a route row (kind='redirect', status=3xx).
-- Caller must separately INSERT the Location into route_headers(route_id, 'Location', target).
-- Demonstrates response header sugar + 3xx status.
-- Usage:
--   INSERT INTO routes SELECT * FROM register_redirect('old','GET','/old','/new',307);
--   INSERT INTO route_headers VALUES ('old', 'Location', '/new');
CREATE OR REPLACE MACRO register_redirect(route_id, method, pattern, target, status := 307) AS TABLE (
  SELECT * FROM register_route(route_id, method, pattern, '', 'redirect', 'Redirect to ' || target, status)
);

-- ============================================================================
-- DEPENDENCIES: request-scoped setup/teardown (FastAPI yield equiv)
-- CREATE DEPENDENCY name AS SETUP '...' TEARDOWN '...'  (via ParserExtension)
-- Attach: CREATE ROUTE ... USING depname AS ...
-- Oracle: dependencies + route_dependencies tables + run_dependency_phase helper.
-- Worker (C) sequences on exec_con: setup; handler; teardown (always).
-- ============================================================================

CREATE OR REPLACE TABLE dependencies (
  name VARCHAR PRIMARY KEY,
  setup_sql VARCHAR,
  teardown_sql VARCHAR
);

CREATE OR REPLACE TABLE route_dependencies (
  route_id VARCHAR,
  dep_name VARCHAR
);

-- run_dependency_phase(dep_name, phase) returns the sql text for that phase.
-- Called by C worker (and tests) to retrieve phase SQL then execute on same con.
-- Use dep_name to avoid column name conflict with dependencies.name .
CREATE OR REPLACE MACRO run_dependency_phase(dep_name, phase) AS (
  CASE phase
    WHEN 'setup' THEN (SELECT setup_sql FROM dependencies d WHERE d.name = dep_name)
    WHEN 'teardown' THEN (SELECT teardown_sql FROM dependencies d WHERE d.name = dep_name)
    ELSE NULL
  END
);

-- ============================================================================
-- AUTH + POLICY registries (v1: JWT HS256 bearer + API_KEY header)
-- Source of truth tables; no precomputed caches. Introspectable and mutable.
-- CREATE AUTH / CREATE POLICY DDL (C++) and register_* (pure) write here.
-- ============================================================================

CREATE OR REPLACE TABLE quackapi_auth (
  name VARCHAR PRIMARY KEY,
  kind VARCHAR,         -- 'jwt_hs256' | 'api_key'
  config_json VARCHAR   -- JSON: for jwt {secret_name, header, verify_exp, leeway}; for apikey {header, table, hash}
);

CREATE OR REPLACE TABLE policies (
  policy_id VARCHAR,
  pattern VARCHAR,         -- e.g. 'POST /admin/*' or 'GET /users/{id}'
  as_mode VARCHAR,         -- 'PERMISSIVE' | 'RESTRICTIVE'
  using_pred VARCHAR,      -- expression over claims/request, e.g. "claims['role']='admin'"
  with_check_pred VARCHAR,
  auth_name VARCHAR        -- which CREATE AUTH to use for verification on matching requests
);

-- api_keys backing store for CREATE AUTH ... AS API_KEY (subject becomes claims['sub'])
-- Created here so handle_request macro (which may reference it inside CASE) parses even when no policies yet.
-- Tests and app DDL may CREATE OR REPLACE / populate; empty by default (no keys → auth fail for apikey schemes).
CREATE OR REPLACE TABLE api_keys (key VARCHAR, subject VARCHAR);

-- register_auth / register_policy: pure-SQL writers (oracle path + tests)
CREATE OR REPLACE MACRO register_auth(name, kind, config_json) AS TABLE (
  SELECT name AS name, kind AS kind, config_json AS config_json
);
CREATE OR REPLACE MACRO register_policy(policy_id, pattern, as_mode, using_pred, with_check_pred := NULL, auth_name := NULL) AS TABLE (
  SELECT policy_id, pattern, as_mode, using_pred, with_check_pred, auth_name
);

-- ── crypto / b64url / JWT helpers (lean on DuckDB 'crypto' ext + json) ──────
-- Load is caller responsibility for JWT paths (tests do LOAD crypto; server workers too when auth used).
-- base64url decode: add padding, map -_ -> +/, from_base64 -> blob
CREATE OR REPLACE MACRO _b64url_decode(s) AS (
  from_base64(
    replace(replace(s, '-', '+'), '_', '/')
    || repeat('=', (4 - (length(s) % 4)) % 4)
  )
);

-- Constant-time string equality. A naive `x = y` on strings is a byte memcmp with an
-- early-out: the time-to-false grows with the shared prefix, a remote timing oracle an
-- attacker walks byte-by-byte to forge an API key or HMAC signature. A true elementwise
-- XOR-fold needs a lambda/unnest over the bytes, which trips DuckDB's "subqueries in
-- lambda" binder inside macro expansion — so instead we compare KEYED HASHES:
-- hmac(salt,x) = hmac(salt,y). One changed byte in x avalanches the whole digest, so
-- digest-prefix timing no longer tracks x's prefix, and forcing the digests equal needs an
-- HMAC 2nd-preimage (infeasible). The salt is a PUBLIC domain-separation constant, not a
-- secret — security rests on HMAC 2nd-preimage resistance, not salt secrecy. Same true/false
-- result as `=`. This is the SINGLE choke point: _verify_jwt_hs256 (sig check) and the
-- api-key lookup both call it, and the C worker invokes this same SQL macro (no separate
-- native compare — brain.cpp:1360), so hardening here hardens every auth path.
CREATE OR REPLACE MACRO _constant_time_str_equals(x, y) AS (
  x IS NOT NULL AND y IS NOT NULL
  AND length(x) = length(y)
  AND crypto_hmac('sha2-256', 'quackapi-ctcmp-domain-v1', x)
    = crypto_hmac('sha2-256', 'quackapi-ctcmp-domain-v1', y)
);

-- Verify HS256 JWT. Returns claims MAP on success, NULL on any failure.
CREATE OR REPLACE MACRO _verify_jwt_hs256(token, secret, verify_exp, leeway) AS (
  WITH
    hh AS (SELECT list_element(string_split(COALESCE(token,''), '.'), 1) AS v),
    pp AS (SELECT list_element(string_split(COALESCE(token,''), '.'), 2) AS v),
    ss AS (SELECT list_element(string_split(COALESCE(token,''), '.'), 3) AS v),
    si AS (SELECT (SELECT v FROM hh) || '.' || (SELECT v FROM pp) AS si, (SELECT v FROM ss) AS sb),
    cb AS (SELECT crypto_hmac('sha2-256', secret, (SELECT si FROM si)) AS b),
    cu AS (SELECT replace(replace(rtrim(base64((SELECT b FROM cb)), '='), '+', '-'), '/', '_') AS c),
    vok AS (SELECT _constant_time_str_equals( (SELECT sb FROM si), (SELECT c FROM cu) ) AS ok),
    pd AS (SELECT CASE WHEN (SELECT ok FROM vok) THEN _b64url_decode((SELECT v FROM pp)) END AS b),
    -- decode(), NOT ::VARCHAR: casting BLOB to VARCHAR renders quote bytes as literal
    -- \x22 escapes, so every real-world payload fails the JSON cast and verification
    -- returns NULL despite a valid signature. decode() reinterprets the bytes as UTF-8.
    pj AS (SELECT CASE WHEN (SELECT b FROM pd) IS NOT NULL THEN try_cast( decode((SELECT b FROM pd)) AS JSON ) END AS j),
    cm AS (SELECT CASE WHEN (SELECT j FROM pj) IS NOT NULL THEN (SELECT j FROM pj)::MAP(VARCHAR, VARCHAR) END AS m),
    ex AS (SELECT CASE WHEN NOT verify_exp OR (SELECT m FROM cm) IS NULL OR (SELECT m FROM cm)['exp'] IS NULL THEN true ELSE (try_cast((SELECT m FROM cm)['exp'] AS BIGINT)+COALESCE(leeway,0)) >= epoch(now()) END AS ok)
  SELECT CASE WHEN (SELECT ok FROM vok) AND (SELECT ok FROM ex) THEN (SELECT m FROM cm) ELSE NULL END
);

-- Auth (JWT via _verify_jwt_hs256 + api_key via api_keys+_constant_time_str_equals) + policy (PERM/REST) enforcement
-- is performed inside handle_request AFTER best (when policies match the request path).
-- Only then: 401 on missing/invalid credential, 403 on valid-cred but policy deny,
-- and successful dynamic handler_sql is wrapped as: WITH _ctx AS (SELECT <claims>::JSON::MAP... , '{}'::JSON AS request) <hsql>
-- No policy match for the route: zero change to prior behavior (unpoliced fast path).

-- Build a request context JSON from the pieces we have at auth time (method, path params, query, body, headers).
-- body may be large; for v1 we pass the raw body string (json or form text).
CREATE OR REPLACE MACRO _build_request_json(method, clean_path, pmap, qmap, headers, body) AS (
  json_object(
    'method', method,
    'path', pmap,
    'query', qmap,
    'body', CASE WHEN body IS NULL OR trim(body)='' THEN NULL ELSE try_cast(body AS JSON) END,
    'headers', headers
  )
);

-- Seed routes from JSON array literal (config-as-data)
INSERT INTO routes
SELECT
  json_extract_string(value, '$.route_id'),
  json_extract_string(value, '$.method'),
  json_extract_string(value, '$.pattern'),
  json_extract_string(value, '$.handler'),
  json_extract_string(value, '$.kind'),
  json_extract_string(value, '$.summary'),
  CASE WHEN try_cast(json_extract_string(value, '$.status') AS INTEGER) IS NULL THEN 200 ELSE try_cast(json_extract_string(value, '$.status') AS INTEGER) END
FROM json_each('[ {"route_id":"get_user","method":"GET","pattern":"/users/{id}","handler":"SELECT to_json(u) AS body FROM users u WHERE u.id = {id}","kind":"dynamic","summary":"Get a user by id","status":200}, {"route_id":"list_users","method":"GET","pattern":"/users","handler":"SELECT coalesce(json_group_array(to_json(u)), ''[]'') AS body FROM users u","kind":"dynamic","summary":"List users","status":200}, {"route_id":"get_post","method":"GET","pattern":"/users/{id}/posts/{post_id}","handler":"SELECT to_json({''user_id'': {id}, ''post_id'': {post_id}}) AS body","kind":"dynamic","summary":"Get a post","status":200}, {"route_id":"search","method":"GET","pattern":"/search","handler":"SELECT coalesce(json_group_array(to_json(u)), ''[]'') AS body FROM (SELECT * FROM users WHERE starts_with(lower(name), lower({q})) ORDER BY id LIMIT coalesce({limit}, 100)) u","kind":"dynamic","summary":"Search","status":200}, {"route_id":"create_user","method":"POST","pattern":"/users","handler":"INSERT INTO users(name, age) VALUES ({name}, {age}) RETURNING to_json(users) AS body","kind":"dynamic","summary":"Create a user","status":201}, {"route_id":"whoami","method":"GET","pattern":"/whoami","handler":"SELECT to_json({''whoami'': rtrim(c.content, chr(10))}) AS body FROM read_text(''whoami |'') c","kind":"dynamic","summary":"Shell: whoami","status":200}, {"route_id":"health","method":"GET","pattern":"/health","handler":"{\"status\":\"ok\"}","kind":"static","summary":"Health check","status":200}, {"route_id":"events","method":"GET","pattern":"/events","handler":"SELECT ''tick '' || i AS body FROM range(1, 6) t(i)","kind":"stream","summary":"SSE event stream demo","status":200}, {"route_id":"openapi","method":"GET","pattern":"/openapi.json","handler":"openapi","kind":"openapi","summary":"OpenAPI schema","status":200}, {"route_id":"docs","method":"GET","pattern":"/docs","handler":"docs","kind":"html","summary":"Swagger UI","status":200} ]'::JSON);

-- Seed param_schema from JSON array literal (config-as-data)
INSERT INTO param_schema
SELECT
  json_extract_string(value, '$.route_id'),
  json_extract_string(value, '$.name'),
  json_extract_string(value, '$.location'),
  json_extract_string(value, '$.type'),
  json_extract_string(value, '$.required')::BOOLEAN,
  json_extract_string(value, '$.constraint_json')
FROM json_each('[ {"route_id":"get_user","name":"id","location":"path","type":"int","required":true,"constraint_json":null}, {"route_id":"get_post","name":"id","location":"path","type":"int","required":true,"constraint_json":null}, {"route_id":"get_post","name":"post_id","location":"path","type":"int","required":true,"constraint_json":null}, {"route_id":"search","name":"q","location":"query","type":"string","required":true,"constraint_json":null}, {"route_id":"search","name":"limit","location":"query","type":"int","required":false,"constraint_json":"{\"le\":100}"}, {"route_id":"create_user","name":"name","location":"body","type":"string","required":true,"constraint_json":null}, {"route_id":"create_user","name":"age","location":"body","type":"int","required":true,"constraint_json":null} ]'::JSON);

-- ============================================================================
-- handle_request — the ONE SQL brain, self-contained. No materialized derived
-- tables: routing, validation, OpenAPI, and Swagger are all computed FROM
-- `routes`/`param_schema` (the registry) per call. This is the PURE track —
-- "DuckDB as far as it goes": the router IS a SQL query over the route registry,
-- exactly as FastAPI's router is a data structure over its decorator registry.
--
-- The only tables in the system are `users` (app data), `routes`, and
-- `param_schema` (the route registry, config-as-data). Nothing is precomputed
-- into a cache table — deliberately. The cost of that honesty is that a
-- parameterized route runs a ~13-operator OLAP query per request (segment match
-- via a window function + list lambdas, param validation, handler templating);
-- that is the wall this track hits (~2.3k req/s, edges.md #9). Crossing it means
-- moving routing OUT of SQL into the C worker / a compiled extension — NOT
-- caching around it with materialized tables. Finding that tear is the point.
--
-- Signature: handle_request(method, path, headers, body)
--   -> (status_code INT, content_type VARCHAR, body VARCHAR, handler_sql VARCHAR, resp_headers VARCHAR)
-- resp_headers added additively (route_headers rows for the matched route_id as JSON object).
-- Existing callers using positional 4-col or named extracts continue to work unchanged.
--
-- The C worker executes handler_sql for dynamic/stream routes and serves `body`
-- directly for static/openapi/html/404/422 (plus resp_headers). In Tier-1 (pure SQL, no server) the
-- caller self-dispatches handler_sql; static/openapi/html bodies come back ready.
--
-- Request surface extensions (R1):
-- - location='header'  : extract from headers_json (key as declared, e.g. 'x-api-key'; C layer lowercases)
-- - location='cookie'  : extract from headers_json._cookies (parsed by C brain.cpp:300 into subobject)
-- - form bodies        : when Content-Type: application/x-www-form-urlencoded, body parsed k=v with url_decode
--                        into same 'body' location params (so {name} from form works identically to json body)
-- - route_headers      : static resp headers (incl Set-Cookie, Location) attached to route_id; emitted in resp_headers
-- - redirect routes    : kind='redirect' + status 3xx + Location in route_headers (register_redirect helper)
-- Validation, 422 shapes, try_cast, constraints identical for header/cookie/form as for query.
-- 422 loc uses the location value directly: ["header","x-api-key"], ["cookie","session"] to match FastAPI.
-- ============================================================================

-- Parse an `a=1&b=2`-style query/form string into a MAP, deduplicating repeated
-- keys by keeping the LAST occurrence (Starlette scalar-param semantics). A bare
-- `map_from_entries` throws "Map keys must be unique" on `?q=a&q=b`, so the
-- dedup is required for crash-safety. `decode` url-decodes keys+values (form
-- bodies) vs leaving them raw (query strings).
CREATE OR REPLACE MACRO _qs_to_map(s, decode) AS (
  SELECT COALESCE(map_from_entries(list(e)), map())
  FROM (
    SELECT DISTINCT ON (k)
      struct_pack(
        key   := CASE WHEN decode THEN url_decode(k) ELSE k END,
        value := CASE WHEN decode THEN url_decode(v) ELSE v END
      ) AS e
    FROM (
      SELECT list_element(string_split(pair, '='), 1) AS k,
             COALESCE(list_element(string_split(pair, '='), 2), '') AS v,
             ord
      FROM unnest(list_filter(string_split(s, '&'), lambda x: len(x) > 0))
        WITH ORDINALITY AS t(pair, ord)
    )
    ORDER BY k, ord DESC
  )
);

CREATE OR REPLACE MACRO handle_request(method, path, headers, body) AS TABLE (
WITH
-- Strip query string for routing (path portion only); parse query string structurally.
-- NOTE: %-decoding of query values is a TODO (not implemented; values passed raw).
path_query AS (
  SELECT
    list_element(string_split(path, '?'), 1) AS clean_path,
    COALESCE(list_element(string_split(path, '?'), 2), '') AS query_str
),
req AS (
  SELECT
    list_filter(string_split(clean_path, '/'), lambda x: len(x) > 0) AS req_segs,
    query_str
  FROM path_query
),
query_map AS (
  SELECT _qs_to_map(req.query_str, false) AS qmap
  FROM req
),
-- form body support (R1): when Content-Type application/x-www-form-urlencoded,
-- parse body k=v&k2=v2 using ONLY string_split + url_decode (built-in, no regex).
-- + becomes space; %XX decoded. Resulting map feeds location='body' params exactly
-- like JSON body path. Pure SQL, matches form semantics for try_cast + constraints.
is_form_ct AS (
  SELECT lower(COALESCE(
    json_extract_string(headers, '$.content-type'),
    json_extract_string(headers, '$.Content-Type'),
    ''
  )) LIKE 'application/x-www-form-urlencoded%' AS yes
),
form_map AS (
  SELECT
    CASE WHEN (SELECT yes FROM is_form_ct)
      THEN _qs_to_map(COALESCE(body, ''), true)
      ELSE map()  -- empty; body params will fall to json path
    END AS fmap
  FROM (SELECT 1)
),
-- ── multipart/form-data support ──────────────────────────────────────────────
-- Detection: Content-Type starts with 'multipart/form-data' AND contains 'boundary='.
-- Boundary extraction: find 'boundary=' in ct_raw, take value up to next ';' or end,
-- strip surrounding double-quotes (quoted boundary per RFC 2046 §5.1.1).
-- Part parsing:
--   Split body by CRLF+'--'+boundary → parts (discard preamble + terminal '--').
--   Each part: split on CRLF CRLF to get (hdr_block, content).
--   Content-Disposition name= and filename= extracted by splitting on ';' then '='.
-- Map building:
--   Non-file parts  → name -> content     (feeds location='body' like form-urlencoded)
--   File parts      → name -> content     (type='file' in param_schema)
--                     name+'__filename' -> filename (auto companion, no schema entry)
-- Binary safety: DuckDB VARCHAR is NOT null-byte safe.  V1 is TEXT-SAFE payloads only.
-- See docs/specs/MULTIPART_SPEC.md §6.
-- ─────────────────────────────────────────────────────────────────────────────
ct_raw AS (
  SELECT COALESCE(
    json_extract_string(headers, '$.content-type'),
    json_extract_string(headers, '$.Content-Type'),
    ''
  ) AS ct
),
is_json_ct AS (
  SELECT
    ( (SELECT ct FROM ct_raw) = ''
      OR lower((SELECT ct FROM ct_raw)) LIKE 'application/%json%'
      OR instr(lower((SELECT ct FROM ct_raw)), '+json') > 0
    ) AS yes
),
is_multipart_ct AS (
  SELECT (
    starts_with(lower((SELECT ct FROM ct_raw)), 'multipart/form-data')
    AND
    -- 'boundary=' substring check via string_split: if split on 'boundary=' yields >1 element
    -- there is a match (no LIKE per project rules — but 'boundary=' is a literal substr check
    -- via list_element which is safe; we avoid LIKE/ILIKE). Use instr instead:
    instr(lower((SELECT ct FROM ct_raw)), 'boundary=') > 0
  ) AS yes
),
-- extract the boundary string from the Content-Type header value.
-- Uses flat scalar expressions only (no nested WITH) to work inside the macro.
-- Strategy: after 'boundary=' in the ct string, clip at ';', strip quotes.
multipart_boundary AS (
  SELECT
    CASE WHEN (SELECT yes FROM is_multipart_ct) THEN
      -- raw_after: text after 'boundary=' (try lowercase key; ct_raw is raw so use original)
      -- We extract using list_element(string_split(ct,'boundary='),2) which gives text after first occurrence.
      -- Then clip at ';' and strip quotes. All flat scalar expressions.
      (SELECT
        CASE
          WHEN starts_with(bnd_trimmed, '"') AND ends_with(bnd_trimmed, '"')
          THEN substr(bnd_trimmed, 2, len(bnd_trimmed) - 2)
          ELSE bnd_trimmed
        END AS bnd
       FROM (SELECT
              trim(
                CASE
                  WHEN instr(after_eq, ';') > 0
                  THEN list_element(string_split(after_eq, ';'), 1)
                  ELSE after_eq
                END
              ) AS bnd_trimmed
             FROM (SELECT
                    COALESCE(
                      list_element(string_split((SELECT ct FROM ct_raw), 'boundary='), 2),
                      list_element(string_split((SELECT ct FROM ct_raw), 'Boundary='), 2),
                      list_element(string_split((SELECT ct FROM ct_raw), 'BOUNDARY='), 2)
                    ) AS after_eq
                  )
             WHERE after_eq IS NOT NULL
           )
      )
    ELSE NULL
    END AS boundary
),
-- Parse the multipart body into a MAP of name->content and name__filename->filename.
-- Uses flat CTE chain as top-level CTEs inside the macro (same level as path_query etc).
-- Strategy: split body by '--' + boundary; skip element 1 (preamble); each subsequent
-- element starts with \r\n (after boundary line) which we strip. Filter out close delimiter.
-- Within each part: split on first \r\n\r\n for hdr+content; parse Content-Disposition.
-- All extraction uses only: string_split, list_*, starts_with, ends_with, substr, instr, trim.
-- No nested WITH inside CASE (not needed; flat lambda expressions do the work).
mp_bnd AS (
  -- alias for boundary for use in subsequent CTEs (avoids repeated subquery)
  SELECT COALESCE((SELECT boundary FROM multipart_boundary), '') AS bnd
),
mp_raw_parts AS (
  -- split body on '--<boundary>'; element 1 = preamble (skip); 2+ = parts
  SELECT
    CASE WHEN (SELECT yes FROM is_multipart_ct) AND (SELECT bnd FROM mp_bnd) <> ''
    THEN list_slice(
           string_split(COALESCE(body, ''), '--' || (SELECT bnd FROM mp_bnd)),
           2, 2147483647
         )
    ELSE []
    END AS raw_parts
),
mp_valid_parts AS (
  -- strip leading \r\n from each part (CRLF after boundary line), filter out '--' closer
  SELECT list_filter(
    list_transform(
      (SELECT raw_parts FROM mp_raw_parts),
      lambda p: CASE WHEN starts_with(p, E'\r\n') THEN substr(p, 3) ELSE p END
    ),
    lambda p: len(p) > 2 AND NOT starts_with(p, '--')
  ) AS parts
),
mp_parsed AS (
  -- unnest valid parts; extract hdr_block and content (split on first \r\n\r\n)
  SELECT
    list_element(string_split(p, E'\r\n\r\n'), 1) AS hdr_block,
    -- content = everything after first \r\n\r\n; strip trailing \r\n if present
    CASE
      WHEN instr(p, E'\r\n\r\n') > 0 THEN
        CASE
          WHEN ends_with(substr(p, instr(p, E'\r\n\r\n') + 4), E'\r\n')
          THEN substr(substr(p, instr(p, E'\r\n\r\n') + 4), 1, len(substr(p, instr(p, E'\r\n\r\n') + 4)) - 2)
          ELSE substr(p, instr(p, E'\r\n\r\n') + 4)
        END
      ELSE ''
    END AS content
  FROM (SELECT unnest(parts) AS p FROM mp_valid_parts)
),
mp_cd AS (
  -- extract Content-Disposition header line and parse name= and filename= from it
  -- cd_line: first header line starting with 'content-disposition' (case-insensitive)
  -- then split on ';' for directives; extract name= and filename= values
  SELECT
    content,
    hdr_block,
    -- find content-disposition line
    list_filter(string_split(hdr_block, E'\r\n'), lambda ln: starts_with(lower(ln), 'content-disposition'))[1] AS cd_line
  FROM mp_parsed
),
mp_directives AS (
  SELECT
    content,
    -- cd_val: value after first ':' in the cd_line
    CASE
      WHEN cd_line IS NOT NULL THEN trim(list_element(string_split(cd_line, ':'), 2))
      ELSE ''
    END AS cd_val
  FROM mp_cd
),
mp_name_filename AS (
  -- extract name= and filename= from the ';'-split directives list
  -- use list_filter to find the entry starting with 'name=' or 'filename='
  -- then extract value after '=' and strip quotes
  SELECT
    content,
    -- part_name: value of name= directive, quotes stripped
    (SELECT
       CASE
         WHEN starts_with(rv, '"') AND ends_with(rv, '"')
         THEN substr(rv, 2, len(rv) - 2)
         ELSE rv
       END
     FROM (SELECT trim(list_element(string_split(
             list_filter(
               list_transform(string_split(cd_val, ';'), lambda d: trim(d)),
               lambda d: starts_with(lower(d), 'name=')
             )[1], '='), 2)) AS rv)
     WHERE rv IS NOT NULL
    ) AS part_name,
    -- part_filename: value of filename= directive, quotes stripped (NULL if absent)
    (SELECT
       CASE
         WHEN starts_with(rv, '"') AND ends_with(rv, '"')
         THEN substr(rv, 2, len(rv) - 2)
         ELSE rv
       END
     FROM (SELECT trim(list_element(string_split(
             list_filter(
               list_transform(string_split(cd_val, ';'), lambda d: trim(d)),
               lambda d: starts_with(lower(d), 'filename=')
             )[1], '='), 2)) AS rv)
     WHERE rv IS NOT NULL
    ) AS part_filename
  FROM mp_directives
),
mp_entries AS (
  -- build (key, value) pairs: content entry + optional filename companion
  SELECT part_name AS k, content AS v
  FROM mp_name_filename
  WHERE part_name IS NOT NULL
  UNION ALL
  SELECT part_name || '__filename' AS k, COALESCE(part_filename, '') AS v
  FROM mp_name_filename
  WHERE part_name IS NOT NULL AND part_filename IS NOT NULL
),
multipart_map AS (
  SELECT
    CASE WHEN (SELECT yes FROM is_multipart_ct)
    THEN COALESCE((SELECT map_from_entries(list(struct_pack(key := k, value := v))) FROM mp_entries), map())
    ELSE map()
    END AS mpmap
  FROM (SELECT 1)
),
-- malformed multipart detection: boundary declared but body has no boundary delimiter
is_malformed_multipart AS (
  SELECT
    (SELECT yes FROM is_multipart_ct)
    AND (SELECT boundary FROM multipart_boundary) IS NOT NULL
    AND instr(COALESCE(body, ''), '--' || (SELECT boundary FROM multipart_boundary)) = 0
  AS yes
),
-- route_idx: split each registered pattern into segments INLINE (no route_index
-- table). seg_count is an O(1) length prefilter; literal_count drives the
-- most-literal-wins tie-break. This is the routing structure FastAPI compiles at
-- startup — here it is recomputed per request, which is exactly the pure-track tax.
route_idx AS (
  -- routes.method is QUALIFIED on purpose: a bare `method` here collides with the
  -- macro's `method` parameter (table-macro substitution would replace the bare
  -- identifier with the argument value, turning the column into a literal).
  SELECT
    route_id, routes.method AS method, pattern, handler, kind, summary, status,
    list_filter(string_split(pattern, '/'), lambda x: len(x) > 0) AS pat_segs,
    len(list_filter(string_split(pattern, '/'), lambda x: len(x) > 0)) AS seg_count,
    len(list_filter(list_filter(string_split(pattern, '/'), lambda x: len(x) > 0), lambda s: NOT starts_with(s, '{'))) AS literal_count
  FROM routes
),
-- path_matches: routes whose path segments match regardless of method.
-- Used for 405 Allow header computation. HEAD auto-included when GET present.
path_matches AS (
  SELECT ri.route_id, ri.method
  FROM route_idx ri, req r
  WHERE ri.seg_count = len(r.req_segs)
    AND len(list_filter(
          list_zip(r.req_segs, ri.pat_segs),
          lambda p: NOT (starts_with(p[2], '{') OR p[1] = p[2])
        )) = 0
),
allow_methods AS (
  SELECT
    CASE WHEN list_contains(list(pm.method), 'GET')
         THEN list_sort(list_distinct(list_concat(list(pm.method), ['HEAD'])))
         ELSE list_sort(list_distinct(list(pm.method)))
    END AS methods,
    array_length(list(pm.method)) > 0 AS path_exists
  FROM path_matches pm
),
-- Structural match: seg_count prefilters by length; the lambda counts position
-- mismatches (a {param} slot matches any segment); most-literal-segments wins
-- ties. No regex.
-- HEAD is matched against GET routes (spec §1.2 Option A): HEAD → treat as GET for lookup.
matched AS (
  SELECT
    ri.route_id, ri.method, ri.pattern, ri.handler, ri.kind, ri.summary, ri.status,
    ri.pat_segs, r.req_segs
  FROM route_idx ri, req r
  WHERE (ri.method = method OR (method = 'HEAD' AND ri.method = 'GET'))
    AND ri.seg_count = len(r.req_segs)
    AND len(list_filter(
          list_zip(r.req_segs, ri.pat_segs),
          lambda p: NOT (starts_with(p[2], '{') OR p[1] = p[2])
        )) = 0
  QUALIFY row_number() OVER (ORDER BY ri.literal_count DESC, ri.route_id) = 1
),
best AS (
  SELECT
    m.*,
    map_from_entries(
      list_transform(
        list_filter(list_zip(m.req_segs, m.pat_segs), lambda p: starts_with(p[2], '{')),
        lambda p: struct_pack(key := substr(p[2], 2, len(p[2]) - 2), value := p[1])
      )
    ) AS pmap
  FROM matched m
),
-- ============================================================================
-- AUTHENTICATE → AUTHORIZE → CLAIMS-BIND (oracle path, parity with C brain.cpp ~1293-1469)
-- Injection after `best` (route_id+pmap available) and before validation/result.
-- If >=1 policy pattern matches the request (segment+method, {}=wild), enforce:
--   * extract cred (Authorization bearer stripped, else X-API-Key variants)
--   * pick scheme from quackapi_auth (header match > jwt-kind > first)
--   * verify: api_key uses `SELECT subject FROM api_keys WHERE _constant_time_str_equals(?,key)`; jwt uses _verify_jwt_hs256
--   *  !vok (no/missing/bad cred) → 401 + {"detail":"Unauthorized"} + handler_sql=NULL
--   *  vok but policies deny (RESTRICTIVE all-true AND + PERMISSIVE any-true OR; restr present → default-deny)
--      → 403 + {"detail":"Forbidden"} + handler_sql=NULL
--   *  vok + allowed and dynamic → wrap hsql: WITH _ctx AS (SELECT '<claims>'::JSON::MAP(VARCHAR,VARCHAR) AS claims, '{}'::JSON AS request) <orig>
--   *  NO matching policy for path → identical prior behavior (no _ctx, no 401/403 from auth)
-- Request json built via _build_request_json (for predicate eval); wrap always uses '{}' for request to match C bytes.
-- ============================================================================
effm AS (
  SELECT CASE WHEN method='HEAD' THEN 'GET' ELSE method END AS m
),
pol_idx AS (
  SELECT
    policy_id, pattern, as_mode, using_pred, with_check_pred, auth_name,
    CASE WHEN instr(pattern,' ')>0 THEN upper(list_element(string_split(pattern,' '),1)) ELSE '' END AS p_m,
    list_filter(string_split( CASE WHEN instr(pattern,' ')>0 THEN COALESCE(list_element(string_split(pattern,' '),2),pattern) ELSE pattern END , '/'), lambda s: len(s)>0) AS p_segs
  FROM policies
),
policy_matches AS (
  SELECT pi.*
  FROM pol_idx pi, req r, effm e
  WHERE len(pi.p_segs) = len(r.req_segs)
    AND (pi.p_m='' OR pi.p_m = upper(e.m))
    AND len(list_filter(list_zip(r.req_segs, pi.p_segs), lambda z: NOT (starts_with(z[2],'{') OR z[1]=z[2]))) = 0
),
nm AS (SELECT count(*) AS n FROM policy_matches),
cred AS (
  SELECT
    CASE WHEN (SELECT n FROM nm)=0 THEN NULL
         ELSE COALESCE(
                -- Authorization (any case key): strip leading/trailing ws + " + optional "Bearer " prefix (case-insens)
                CASE
                  WHEN COALESCE(json_extract_string(headers,'$.authorization'),json_extract_string(headers,'$.Authorization')) IS NOT NULL THEN
                    (WITH raw AS (SELECT COALESCE(json_extract_string(headers,'$.authorization'),json_extract_string(headers,'$.Authorization')) AS r),
                          nob AS (SELECT trim(replace((SELECT r FROM raw), '"', '')) AS s),
                          up AS (SELECT upper((SELECT s FROM nob)) AS u, (SELECT s FROM nob) AS s),
                          cut AS (SELECT CASE WHEN starts_with((SELECT u FROM up), 'BEARER ') THEN substr((SELECT s FROM up), 8) ELSE (SELECT s FROM up) END AS t)
                     SELECT trim((SELECT t FROM cut)))
                  ELSE NULL
                END,
                trim(replace(COALESCE(json_extract_string(headers,'$.x-api-key'),json_extract_string(headers,'$.X-API-Key'),json_extract_string(headers,'$.x_api_key'),''), '"', ''))
              )
    END AS tok
),
sch AS (
  SELECT name, kind, config_json,
    json_extract_string(config_json,'$.header') AS cfg_h,
    json_extract_string(config_json,'$.secret') AS secret,
    COALESCE(try_cast(json_extract_string(config_json,'$.verify_exp')AS BOOLEAN), true) AS verify_exp,
    COALESCE(try_cast(json_extract_string(config_json,'$.leeway')AS INTEGER), 0) AS leeway
  FROM quackapi_auth
  WHERE (SELECT n FROM nm)>0
  ORDER BY
    -- Prefer the scheme whose KIND matches the credential header actually present in
    -- this request (kind is the reliable discriminator, not a substring of the header
    -- name). Exact equality on the known enum ('jwt_hs256' | 'api_key') — no LIKE.
    CASE
      WHEN (SELECT tok FROM cred) IS NOT NULL AND kind = 'api_key'   AND instr(lower(headers),'x-api-key')     > 0 THEN 0
      WHEN (SELECT tok FROM cred) IS NOT NULL AND kind = 'jwt_hs256' AND instr(lower(headers),'authorization') > 0 THEN 0
      ELSE 1
    END,
    CASE WHEN kind = 'jwt_hs256' THEN 0 ELSE 1 END,
    name
  LIMIT 1
),
vok AS (
  SELECT
    (SELECT n FROM nm)>0 AND (SELECT tok FROM cred) IS NOT NULL AND (SELECT tok FROM cred)<>'' AND (SELECT kind FROM sch) IS NOT NULL AND
    CASE
      WHEN (SELECT kind FROM sch) = 'api_key' THEN
        EXISTS(SELECT 1 FROM api_keys k WHERE _constant_time_str_equals((SELECT tok FROM cred), k.key))
      ELSE
        _verify_jwt_hs256( (SELECT tok FROM cred), COALESCE((SELECT secret FROM sch),''), COALESCE((SELECT verify_exp FROM sch),true), COALESCE((SELECT leeway FROM sch),0) ) IS NOT NULL
    END AS ok,
    CASE
      WHEN (SELECT kind FROM sch) = 'api_key' THEN
        (SELECT to_json(map_from_entries([struct_pack(key:='sub',value:=COALESCE(k.subject,''))]))::VARCHAR FROM api_keys k WHERE _constant_time_str_equals((SELECT tok FROM cred),k.key) LIMIT 1)
      ELSE
        (SELECT to_json( _verify_jwt_hs256((SELECT tok FROM cred), COALESCE((SELECT secret FROM sch),''), COALESCE((SELECT verify_exp FROM sch),true), COALESCE((SELECT leeway FROM sch),0)) )::VARCHAR )
    END AS cj
),
-- Policy predicate eval — LITERALS ONLY on the oracle/pure-track, by architectural
-- necessity: a SQL macro cannot EXECUTE a dynamic predicate expression over runtime
-- claims (the same limit that forces self-dispatch for handler execution). So a literal
-- ''/true/1 → allow, false/0 → deny, and any NON-literal predicate (e.g. the common
-- `claims['sub'] IS NOT NULL` "require authenticated user" idiom, or `claims['role']='admin'`)
-- FAIL-CLOSES to deny (pass=false → 403). This is safe (never grants on an unevaluatable
-- predicate) but is NOT parity with the compiled ext-cpp track, which evaluates the full
-- predicate via a prepared statement. Full pure-track predicate eval would require a
-- self-dispatched auth-check query. See docs/AUTH_ORACLE_WIRING_RESULT.md "HONEST BOUNDARY".
pol_p AS (
  SELECT
    pm.policy_id,
    upper(COALESCE(pm.as_mode,'PERMISSIVE')) AS mode,
    CASE
      WHEN pm.using_pred IS NULL OR trim(pm.using_pred)='' OR lower(trim(pm.using_pred)) IN ('true','1') THEN true
      WHEN lower(trim(pm.using_pred)) IN ('false','0') THEN false
      ELSE false  -- non-literal predicate: fail-closed (deny). Compiled track does full eval.
    END AS pass
  FROM policy_matches pm
),
pol_a AS (
  SELECT
    (SELECT n FROM nm)>0 AS has_pol,
    bool_or(mode='RESTRICTIVE') AS has_restr,
    list_count(list_filter(list(CASE WHEN mode='RESTRICTIVE' THEN (CASE WHEN pass THEN 1 ELSE 0 END) ELSE NULL END), x->x IS NOT NULL)) AS restr_n,
    list_count(list_filter(list(CASE WHEN mode='RESTRICTIVE' THEN (CASE WHEN pass THEN 1 ELSE 0 END) ELSE NULL END), x->x=0)) = 0 AS all_restr,
    bool_or(mode<>'RESTRICTIVE') AS has_perm,
    bool_or(mode<>'RESTRICTIVE' AND pass) AS any_perm,
    (SELECT ok FROM vok) AS vok,
    (SELECT cj FROM vok) AS claims_j,
    (SELECT _build_request_json((SELECT m FROM effm), (SELECT clean_path FROM path_query), COALESCE((SELECT pmap FROM best),map()), (SELECT qmap FROM query_map), headers, body) FROM (SELECT 1)) AS req_j
  FROM pol_p
),
auth_dec AS (
  SELECT
    has_pol,
    vok,
    claims_j,
    CASE WHEN has_pol AND NOT vok THEN 401
         WHEN has_pol AND vok THEN (CASE
           WHEN has_restr THEN (CASE WHEN all_restr AND (NOT has_perm OR any_perm) THEN 200 ELSE 403 END)
           ELSE (CASE WHEN NOT has_perm OR any_perm THEN 200 ELSE 403 END)
         END)
         ELSE 0
    END AS forced_status,
    CASE WHEN has_pol AND NOT vok THEN '{"detail":"Unauthorized"}'
         WHEN has_pol AND vok AND ( (has_restr AND NOT (all_restr AND (NOT has_perm OR any_perm))) OR (NOT has_restr AND has_perm AND NOT any_perm) ) THEN '{"detail":"Forbidden"}'
         ELSE NULL
    END AS forced_body
  FROM pol_a
),
-- Extract values for path/query/header/cookie/body params of the matched route.
-- header: top-level key in headers_json (C provides lowercased names)
-- cookie: nested under _cookies sub-object (already parsed by C layer into headers_json)
-- body:  if multipart ct then from multipart_map (mpmap); if form ct then from form_map;
--        else json_extract (back-compat). Multipart takes priority over form.
-- All flow into same val_str + validation pipeline (try_cast + constraints + required).
-- type='file': value is raw file content (no type coercion — skip try_cast + constraints).
param_values AS (
  SELECT
    ps.route_id,
    ps.name,
    ps.location,
    ps.type,
    ps.required,
    ps.constraint_json,
    CASE ps.location
      WHEN 'path'  THEN b.pmap[ps.name]
      WHEN 'query' THEN qm.qmap[ps.name]
      WHEN 'header' THEN json_extract_string(headers, '$.' || ps.name)
      WHEN 'cookie' THEN
        CASE WHEN json_extract(headers, '$._cookies') IS NULL THEN NULL
             ELSE json_extract_string(json_extract(headers, '$._cookies'), '$.' || ps.name)
        END
      WHEN 'body'  THEN
        CASE
          WHEN body IS NULL OR trim(body) = '' THEN NULL
          WHEN (SELECT yes FROM is_multipart_ct) THEN mpm.mpmap[ps.name]
          WHEN (SELECT yes FROM is_form_ct) THEN fm.fmap[ps.name]
          WHEN NOT (SELECT yes FROM is_json_ct) THEN NULL
          -- try(): a non-JSON body must yield 422 (via missing/validation), never
          -- an Invalid Input Error that aborts the whole request.
          ELSE try(json_extract_string(body, '$.' || ps.name))
        END
      ELSE NULL
    END AS val_str,
    -- body_raw: for body json only, the raw JSON value (to distinguish absent key vs explicit null/wrong-type for string_type etc). sql NULL if absent or non-json-body.
    CASE WHEN ps.location = 'body'
              AND body IS NOT NULL AND trim(body) <> ''
              AND NOT (SELECT yes FROM is_multipart_ct) AND NOT (SELECT yes FROM is_form_ct)
              AND (SELECT yes FROM is_json_ct)
         THEN try(json_extract(body, '$.' || ps.name))
         ELSE NULL
    END AS body_raw
  FROM param_schema ps
  JOIN best b ON ps.route_id = b.route_id
  CROSS JOIN query_map qm
  CROSS JOIN form_map fm
  CROSS JOIN multipart_map mpm
),
validation_errors AS (
  SELECT
    pv.name,
    pv.location,
    pv.type,
    pv.required,
    pv.constraint_json,
    pv.val_str,
    pv.body_raw,
    CASE
      -- string_type for wrong-type on string (e.g. explicit null or number provided for a string field in JSON body)
      -- present (body_raw not sql-null) but json type not VARCHAR (covers null, num, bool, etc). FastAPI uses string_type.
      WHEN pv.type = 'string'
           AND pv.location = 'body'
           AND pv.body_raw IS NOT NULL
           AND json_type(pv.body_raw) <> 'VARCHAR'
        THEN 'string_type'
      WHEN pv.required AND pv.val_str IS NULL THEN 'missing'
      -- type='file': no coercion (raw content; may be any text). Only required check applies.
      WHEN pv.type = 'file' THEN NULL
      -- Strict integer parse (FastAPI/Pydantic semantics). DuckDB's own cast is
      -- lenient: '1.5'::INT rounds to 2 and '1e2'::INT parses to 100, both of
      -- which Pydantic rejects. So a value is a valid int ONLY if it casts to a
      -- (wide) integer AND carries no decimal point and no exponent marker.
      -- string_split is a pure builtin (no regex/LIKE, per project rules).
      -- HUGEINT (128-bit) widens the accepted range toward Python's unbounded int.
      WHEN pv.type = 'int' AND pv.val_str IS NOT NULL AND (
             try_cast(pv.val_str AS HUGEINT) IS NULL
             OR len(string_split(lower(pv.val_str), '.')) > 1
             OR len(string_split(lower(pv.val_str), 'e')) > 1
           ) THEN 'int_parsing'
      WHEN pv.type = 'float' AND pv.val_str IS NOT NULL AND try_cast(pv.val_str AS DOUBLE) IS NULL THEN 'float_parsing'
      WHEN pv.type = 'bool' AND pv.val_str IS NOT NULL AND try_cast(pv.val_str AS BOOLEAN) IS NULL THEN 'bool_parsing'
      ELSE NULL
    END AS err_code,
    CASE
      -- type='file': no constraints apply
      WHEN pv.type = 'file' THEN NULL
      WHEN pv.type = 'int' AND pv.val_str IS NOT NULL AND try_cast(pv.val_str AS BIGINT) IS NOT NULL AND pv.constraint_json IS NOT NULL THEN
        CASE
          WHEN json_extract_string(pv.constraint_json, '$.le') IS NOT NULL
               AND try_cast(pv.val_str AS BIGINT) > try_cast(json_extract_string(pv.constraint_json, '$.le') AS BIGINT)
          THEN 'less_than_equal'
          WHEN json_extract_string(pv.constraint_json, '$.ge') IS NOT NULL
               AND try_cast(pv.val_str AS BIGINT) < try_cast(json_extract_string(pv.constraint_json, '$.ge') AS BIGINT)
          THEN 'greater_than_equal'
          ELSE NULL
        END
      ELSE NULL
    END AS constr_err_code
  FROM param_values pv
),
err_rows AS (
  SELECT
    name,
    location,
    type,
    required,
    constraint_json,
    val_str,
    COALESCE(err_code, constr_err_code) AS err_code,
    err_code AS type_err_code,
    constr_err_code
  FROM validation_errors
  WHERE COALESCE(err_code, constr_err_code) IS NOT NULL
),
err_agg AS (
  SELECT count(*) AS n_err, '[' || list_aggr(
    list(
      -- Build the detail object as a JSON string so we can conditionally append
      -- the FastAPI "ctx" field only for constraint-violation error types.
      -- Field order: type, loc, msg, input[, ctx]  (matches FastAPI pydantic-v2).
      -- Strategy: json_object closes with "}"; for constraint errors we strip the
      -- trailing "}" with left(...,-1) and reclose after the ctx fragment.
      CASE er.err_code
        WHEN 'less_than_equal' THEN
          left(json_object(
            'type', er.err_code,
            'loc',  json_array(er.location, er.name),
            'msg',  'Input should be less than or equal to ' || COALESCE(json_extract_string(er.constraint_json, '$.le'), ''),
            'input', er.val_str
          ), -1)
          || ',"ctx":{"le":' || COALESCE(json_extract_string(er.constraint_json, '$.le'), 'null') || '}}'
        WHEN 'greater_than_equal' THEN
          left(json_object(
            'type', er.err_code,
            'loc',  json_array(er.location, er.name),
            'msg',  'Input should be greater than or equal to ' || COALESCE(json_extract_string(er.constraint_json, '$.ge'), ''),
            'input', er.val_str
          ), -1)
          || ',"ctx":{"ge":' || COALESCE(json_extract_string(er.constraint_json, '$.ge'), 'null') || '}}'
        ELSE
          json_object(
            'type', er.err_code,
            'loc',  json_array(er.location, er.name),
            'msg',  CASE er.err_code
                      WHEN 'missing'       THEN 'Field required'
                      WHEN 'int_parsing'   THEN 'Input should be a valid integer, unable to parse string as an integer'
                      WHEN 'float_parsing' THEN 'Input should be a valid number, unable to parse string as a number'
                      WHEN 'bool_parsing'  THEN 'Input should be a valid boolean, unable to parse string as a boolean'
                      WHEN 'string_type'   THEN 'Input should be a valid string'
                    END,
            'input', er.val_str
          )
      END
    ), 'string_agg', ','
  ) || ']' AS detail_arr
  FROM err_rows er
),
-- Handler rendering: substitute {param} -> SQL literal via replace + list_reduce
-- (no regex). Drives only dynamic/stream routes; static/openapi/html bodies come
-- from rendered_static below.
param_literals AS (
  SELECT
    pv.name,
    pv.type,
    pv.val_str,
    CASE
      WHEN pv.val_str IS NOT NULL THEN
        CASE pv.type
          WHEN 'int' THEN pv.val_str
          WHEN 'float' THEN pv.val_str
          WHEN 'bool' THEN lower(pv.val_str)
          -- type='file': pass raw content as SQL string literal (same as string)
          ELSE '''' || replace(pv.val_str, '''', '''''') || ''''
        END
      ELSE 'NULL'
    END AS literal
  FROM param_values pv
),
-- multipart companion literals: expose <name>__filename from mpmap for handler template
-- substitution WITHOUT requiring a param_schema entry. These are string literals.
-- Only active when multipart is in play (mpmap is non-empty).
multipart_companion_literals AS (
  SELECT
    ue.k AS name,
    'string' AS type,
    ue.v AS val_str,
    '''' || replace(ue.v, '''', '''''') || '''' AS literal
  FROM (
    SELECT unnest(map_keys(mpmap)) AS k, unnest(map_values(mpmap)) AS v
    FROM multipart_map
  ) ue
  -- only the companion __filename entries (not the content entries which are in param_schema)
  WHERE ends_with(ue.k, '__filename')
    AND (SELECT yes FROM is_multipart_ct)
),
param_list AS (
  SELECT list(struct_pack(name := name, literal := literal)) AS plist
  FROM (
    SELECT name, literal FROM param_literals
    UNION ALL
    SELECT name, literal FROM multipart_companion_literals
  )
),
handler_rendered AS (
  SELECT
    ( list_reduce(
        COALESCE(
          list_transform( COALESCE( (SELECT plist FROM param_list), []::STRUCT(name VARCHAR, literal VARCHAR)[] ), lambda p: struct_pack(s := '', name := p.name, literal := p.literal) ),
          []::STRUCT(s VARCHAR, name VARCHAR, literal VARCHAR)[]
        ),
        lambda acc, stp: struct_pack( s := replace(acc.s, '{' || stp.name || '}', stp.literal), name := '', literal := '' ),
        struct_pack( s := (SELECT handler FROM best), name := '', literal := '' )
      )
    ).s AS hsql
),
-- rendered_static: openapi / docs / static bodies, built INLINE from routes +
-- param_schema for the matched route ONLY. `best` has <=1 row and the WHERE keeps
-- only static-ish kinds, so the (heavier) OpenAPI build never evaluates on the
-- dynamic hot path — it runs solely when the matched route IS /openapi.json.
-- This replaces the old response_cache table: the OpenAPI doc is a SELECT, not a
-- materialized string (openapi-as-a-query, the original pillar).
rendered_static AS (
  SELECT
    b.route_id,
    CASE b.kind WHEN 'html' THEN 'text/html' ELSE 'application/json' END AS content_type,
    CASE b.kind
      WHEN 'openapi' THEN (
        SELECT CAST(json_object(
          'openapi', '3.0.0',
          'info', json_object('title', 'quackapi', 'version', '0.1.0'),
          'paths', json_group_object(pm.pattern, pm.methods_map)
        ) AS VARCHAR)
        FROM (
          SELECT owp.pattern, json_group_object(owp.meth, owp.op_obj) AS methods_map
          FROM (
            SELECT
              r2.pattern,
              lower(r2.method) AS meth,
              json_object(
                'summary', r2.summary,
                'parameters', COALESCE((
                  SELECT json_group_array(json_object(
                    'name', ps.name,
                    'in', ps.location,
                    'required', ps.required,
                    'schema', json_object('type',
                      CASE ps.type
                        WHEN 'int' THEN 'integer'
                        WHEN 'float' THEN 'number'
                        WHEN 'bool' THEN 'boolean'
                        ELSE 'string'
                      END)
                  ))
                  FROM param_schema ps
                  WHERE ps.route_id = r2.route_id AND ps.location IN ('path', 'query', 'header', 'cookie')
                ), '[]'::JSON),
                'responses', json_object(
                  CAST(r2.status AS VARCHAR), json_object('description', CASE WHEN r2.status = 201 THEN 'Created' ELSE 'OK' END, 'content', json_object('application/json', json_object('schema', json_object('type', 'object')))),
                  '422', json_object('description', 'Validation Error', 'content', json_object('application/json', json_object('schema', json_object('type', 'object'))))
                )
              ) AS op_obj
            FROM routes r2
          ) owp
          GROUP BY owp.pattern
        ) pm
      )
      WHEN 'html' THEN '<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><title>quackapi - Swagger UI</title><link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css"></head><body><div id="swagger-ui"></div><script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js" charset="UTF-8"></script><script>SwaggerUIBundle({url:"/openapi.json",dom_id:"#swagger-ui",presets:[SwaggerUIBundle.presets.apis],layout:"BaseLayout"});</script></body></html>'
      WHEN 'static' THEN b.handler
      ELSE NULL
    END AS body
  FROM best b
  WHERE b.kind IN ('openapi', 'html', 'static')
),
-- Collapse to ONE row: LEFT JOIN best/err_agg/rendered_static/handler_rendered so
-- the final projection reads COLUMNS, not repeated scalar subqueries that each
-- re-run routing. best executes once.
-- is_malformed_multipart carries into result for short-circuit 422 (structural body error).
result AS (
  SELECT b.route_id, b.kind, b.status, ea.n_err, ea.detail_arr, rs.content_type AS rc_ct, rs.body AS rc_body, hr.hsql,
    -- resp_headers from route_headers side table (additive). For 404 -> '{}'; for matched (incl 422 errs) emit the route's static headers.
    COALESCE((SELECT json_group_object(name, value) FROM route_headers WHERE route_id = b.route_id), '{}') AS resp_headers,
    (SELECT yes FROM is_malformed_multipart) AS malformed_mp,
    ad.forced_status, ad.forced_body, ad.has_pol, ad.vok, ad.claims_j
  FROM (SELECT 1) z
  LEFT JOIN best b ON true
  LEFT JOIN err_agg ea ON true
  LEFT JOIN rendered_static rs ON rs.route_id = b.route_id
  LEFT JOIN handler_rendered hr ON true
  LEFT JOIN auth_dec ad ON true
)
SELECT
  -- Auth decisions override route/validation decisions (C does the same after quack_route)
  CASE
    WHEN (SELECT forced_status FROM result WHERE forced_status IN (401,403) LIMIT 1) = 401 THEN 401
    WHEN (SELECT forced_status FROM result WHERE forced_status IN (401,403) LIMIT 1) = 403 THEN 403
    WHEN malformed_mp THEN 422
    WHEN route_id IS NULL AND (SELECT path_exists FROM allow_methods) THEN 405
    WHEN route_id IS NULL THEN 404
    WHEN n_err > 0 THEN 422
    ELSE status
  END AS status_code,
  CASE
    WHEN (SELECT forced_status FROM result WHERE forced_status IN (401,403) LIMIT 1) IN (401,403) THEN 'application/json'
    WHEN malformed_mp OR route_id IS NULL OR n_err > 0 THEN 'application/json'
    WHEN kind IN ('openapi', 'static', 'html') THEN rc_ct
    WHEN kind = 'stream' THEN 'text/event-stream'
    ELSE 'application/json'
  END AS content_type,
  CASE
    WHEN (SELECT forced_status FROM result WHERE forced_status=401 LIMIT 1) = 401 THEN (SELECT forced_body FROM result LIMIT 1)
    WHEN (SELECT forced_status FROM result WHERE forced_status=403 LIMIT 1) = 403 THEN (SELECT forced_body FROM result LIMIT 1)
    WHEN malformed_mp THEN cast(json_object('detail', json_array(
        json_object('type', 'multipart_parse', 'loc', json_array('body'), 'msg', 'Malformed multipart body: boundary not found')
      )) AS VARCHAR)
    WHEN route_id IS NULL AND (SELECT path_exists FROM allow_methods)
      THEN cast(json_object('detail', 'Method Not Allowed') AS VARCHAR)
    WHEN route_id IS NULL THEN cast(json_object('detail', 'Not Found') AS VARCHAR)
    WHEN n_err > 0 THEN '{"detail":' || detail_arr || '}'
    WHEN kind IN ('openapi', 'static', 'html') THEN rc_body
    WHEN kind = 'redirect' THEN NULL
    ELSE NULL
  END AS body,
  CASE
    WHEN (SELECT forced_status FROM result WHERE forced_status IN (401,403) LIMIT 1) IN (401,403) THEN NULL
    WHEN malformed_mp OR route_id IS NULL OR n_err > 0 THEN NULL
    WHEN kind IN ('dynamic', 'stream') THEN
      CASE
        -- wrap ONLY if a policy matched this path AND we are emitting a handler_sql (dynamic) AND auth passed
        WHEN (SELECT has_pol FROM result LIMIT 1) AND (SELECT vok FROM result LIMIT 1) AND hsql IS NOT NULL THEN
          'WITH _ctx AS (SELECT ''' || replace(COALESCE((SELECT claims_j FROM result LIMIT 1), '{}'), '''', '''''') || '''::JSON::MAP(VARCHAR,VARCHAR) AS claims, ''{}''::JSON AS request) ' || hsql
        ELSE hsql
      END
    WHEN kind = 'redirect' THEN NULL
    ELSE NULL
  END AS handler_sql,
  CASE
    WHEN route_id IS NULL AND (SELECT path_exists FROM allow_methods)
      THEN json_object('Allow', array_to_string((SELECT methods FROM allow_methods), ', '))
    ELSE resp_headers
  END AS resp_headers
FROM result
);

-- GATE self-checks (must produce expected results). Dynamic routes return handler_sql (col4),
-- body=NULL (the C layer executes handler_sql); 404/422/static/openapi/html return body, handler_sql=NULL.
SELECT * FROM handle_request('GET','/users/123','{}','');
SELECT * FROM handle_request('GET','/users/abc','{}','');
SELECT * FROM handle_request('GET','/nope/here','{}','');
SELECT * FROM handle_request('GET','/users/7/posts/99','{}','');
SELECT * FROM handle_request('GET','/search?q=hi&limit=5','{}','');
SELECT * FROM handle_request('GET','/search?q=hi&limit=abc','{}','');
SELECT * FROM handle_request('GET','/search?q=hi&limit=999','{}','');
SELECT * FROM handle_request('GET','/search?limit=5','{}','');
SELECT * FROM handle_request('POST','/users','{}','{"name":"al","age":30}');
SELECT * FROM handle_request('POST','/users','{}','{"name":"al"}');
SELECT status_code, content_type, substr(body,1,60), handler_sql IS NULL FROM handle_request('GET','/openapi.json','{}','');
SELECT status_code, content_type, substr(body,1,40), handler_sql IS NULL FROM handle_request('GET','/docs','{}','');
SELECT json_extract_string((SELECT body FROM handle_request('GET','/openapi.json','{}','')), '$.openapi') AS v;
SELECT status_code, content_type, body, handler_sql IS NULL FROM handle_request('GET','/health','{}','');
