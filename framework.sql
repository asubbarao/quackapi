-- ============================================================================
-- quackapi bootstrap — extensions + HTTP CLIENT POLICY (curl_httpfs is the law)
-- ============================================================================
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

-- register_route macro: app.sql calls this to produce route rows (no VALUES-literal hardcode of config rows in app).
-- Usage: INSERT INTO routes SELECT * FROM register_route('id', 'GET', '/p', 'HANDLER', 'dynamic', 'sum', 200);
CREATE OR REPLACE MACRO register_route(route_id, method, pattern, handler, kind, summary, status := 200) AS TABLE (
  SELECT
    route_id AS route_id,
    method,
    pattern,
    handler,
    kind,
    summary,
    status
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
FROM json_each('[ {"route_id":"get_user","method":"GET","pattern":"/users/{id}","handler":"SELECT to_json(u) AS body FROM users u WHERE u.id = {id}","kind":"dynamic","summary":"Get a user by id","status":200}, {"route_id":"list_users","method":"GET","pattern":"/users","handler":"SELECT coalesce(json_group_array(to_json(u)), ''[]'') AS body FROM users u","kind":"dynamic","summary":"List users","status":200}, {"route_id":"get_post","method":"GET","pattern":"/users/{id}/posts/{post_id}","handler":"SELECT to_json({''user_id'': {id}, ''post_id'': {post_id}}) AS body","kind":"dynamic","summary":"Get a post","status":200}, {"route_id":"search","method":"GET","pattern":"/search","handler":"SELECT coalesce(json_group_array(to_json(u)), ''[]'') AS body FROM (SELECT * FROM users WHERE starts_with(lower(name), lower({q})) ORDER BY id LIMIT coalesce({limit}, 100)) u","kind":"dynamic","summary":"Search","status":200}, {"route_id":"create_user","method":"POST","pattern":"/users","handler":"INSERT INTO users(name, age) VALUES ({name}, {age}) RETURNING to_json(users) AS body","kind":"dynamic","summary":"Create a user","status":201}, {"route_id":"whoami","method":"GET","pattern":"/whoami","handler":"SELECT to_json({''whoami'': rtrim(c.content, chr(10))}) AS body FROM read_text(''whoami |'') c","kind":"dynamic","summary":"Shell: whoami","status":200}, {"route_id":"openapi","method":"GET","pattern":"/openapi.json","handler":"openapi","kind":"openapi","summary":"OpenAPI schema","status":200}, {"route_id":"docs","method":"GET","pattern":"/docs","handler":"docs","kind":"html","summary":"Swagger UI","status":200} ]'::JSON);

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

-- The router + request handler as TABLE MACRO
CREATE OR REPLACE MACRO handle_request(method, path, headers, body) AS TABLE (
WITH
-- Strip query string for routing (path portion only); parse query string structurally.
-- NOTE: %-decoding of query values is a TODO (not implemented; values passed raw).
req_raw AS (SELECT path AS full_path),
path_query AS (
  SELECT
    list_element(string_split(full_path, '?'), 1) AS clean_path,
    COALESCE(list_element(string_split(full_path, '?'), 2), '') AS query_str
  FROM req_raw
),
req AS (
  SELECT
    list_filter(string_split(pq.clean_path, '/'), lambda x: len(x) > 0) AS req_segs,
    pq.query_str
  FROM path_query pq
),
query_map AS (
  SELECT map_from_entries(
    list_transform(
      list_filter(string_split(req.query_str, '&'), lambda x: len(x) > 0),
      lambda pair: struct_pack(
        key := list_element(string_split(pair, '='), 1),
        value := COALESCE(list_element(string_split(pair, '='), 2), '')
      )
    )
  ) AS qmap
  FROM req
),
candidates AS (
  SELECT
    r.route_id,
    r.method,
    r.pattern,
    r.kind,
    r.summary,
    r.status,
    list_filter(string_split(r.pattern, '/'), lambda x: len(x) > 0) AS pat_segs,
    req.req_segs AS req_segs
  FROM routes r
  CROSS JOIN req
  WHERE r.method = method
),
matched AS (
  SELECT
    c.route_id,
    c.method,
    c.pattern,
    c.kind,
    c.summary,
    c.status,
    c.pat_segs,
    c.req_segs,
    (len(c.req_segs) = len(c.pat_segs)) AS len_match,
    CASE
      WHEN len(c.req_segs) = len(c.pat_segs) THEN
        len( list_filter(
          list_zip(c.req_segs, c.pat_segs),
          lambda p: NOT (starts_with(p[2], '{') OR p[1] = p[2])
        ))
      ELSE 999
    END AS mismatch_count,
    (len(c.pat_segs) - len(list_filter(c.pat_segs, lambda s: starts_with(s, '{')))) AS literal_count
  FROM candidates c
),
best_match AS (
  SELECT * FROM matched
  WHERE len_match AND mismatch_count = 0
  QUALIFY row_number() OVER (ORDER BY literal_count DESC, route_id) = 1
),
best AS (
  SELECT
    bm.*,
    map_from_entries(
      list_transform(
        list_filter(
          list_zip(bm.req_segs, bm.pat_segs),
          lambda p: starts_with(p[2], '{')
        ),
        lambda p: struct_pack(key := substr(p[2], 2, len(p[2]) - 2), value := p[1])
      )
    ) AS pmap
  FROM best_match bm
),
-- Extract values for path/query/body params of the matched route.
-- Body uses json_extract_string when body non-empty; query from the parsed map.
param_values AS (
  SELECT
    ps.route_id,
    ps.name,
    ps.location,
    ps.type,
    ps.required,
    ps.constraint_json,
    b.pmap,
    qm.qmap,
    body AS body_json,
    CASE ps.location
      WHEN 'path' THEN b.pmap[ps.name]
      WHEN 'query' THEN qm.qmap[ps.name]
      WHEN 'body' THEN CASE WHEN body IS NULL OR len(body) = 0 THEN NULL ELSE json_extract_string(body, '$.' || ps.name) END
      ELSE NULL
    END AS val_str
  FROM param_schema ps
  JOIN best b ON ps.route_id = b.route_id
  CROSS JOIN query_map qm
),
validation_errors AS (
  SELECT
    pv.name,
    pv.location,
    pv.type,
    pv.required,
    pv.constraint_json,
    pv.val_str,
    CASE
      WHEN pv.required AND pv.val_str IS NULL THEN 'missing'
      WHEN pv.type = 'int' AND pv.val_str IS NOT NULL AND try_cast(pv.val_str AS BIGINT) IS NULL THEN 'int_parsing'
      WHEN pv.type = 'float' AND pv.val_str IS NOT NULL AND try_cast(pv.val_str AS DOUBLE) IS NULL THEN 'float_parsing'
      WHEN pv.type = 'bool' AND pv.val_str IS NOT NULL AND try_cast(pv.val_str AS BOOLEAN) IS NULL THEN 'bool_parsing'
      ELSE NULL
    END AS err_code,
    CASE
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
err_json AS (
  SELECT json_group_array(
    json_object(
      'type', er.err_code,
      'loc', json_array(er.location, er.name),
      'msg', CASE er.err_code
               WHEN 'missing' THEN 'Field required'
               WHEN 'int_parsing' THEN 'Input should be a valid integer, unable to parse string as an integer'
               WHEN 'float_parsing' THEN 'Input should be a valid number, unable to parse string as a number'
               WHEN 'bool_parsing' THEN 'Input should be a valid boolean, unable to parse string as a boolean'
               WHEN 'less_than_equal' THEN 'Input should be less than or equal to ' || COALESCE(json_extract_string(er.constraint_json, '$.le'), '')
               WHEN 'greater_than_equal' THEN 'Input should be greater than or equal to ' || COALESCE(json_extract_string(er.constraint_json, '$.ge'), '')
             END
    )
  ) AS detail_arr
  FROM err_rows er
),
casted_params AS (
  SELECT
    pv.name,
    CASE pv.type
      WHEN 'int' THEN to_json(try_cast(pv.val_str AS BIGINT))
      WHEN 'float' THEN to_json(try_cast(pv.val_str AS DOUBLE))
      WHEN 'bool' THEN to_json(try_cast(pv.val_str AS BOOLEAN))
      ELSE to_json(pv.val_str)
    END AS val
  FROM param_values pv
  WHERE pv.val_str IS NOT NULL
),
params_obj AS (
  SELECT json_group_object(name, val) AS params_json FROM casted_params
),
-- OpenAPI 3.0 document built by querying routes + param_schema (no hardcode).
-- Generation is a query over tables (strictly easier than FastAPI introspection).
ops AS (
  SELECT
    r.pattern,
    lower(r.method) AS meth,
    r.summary,
    r.route_id,
    r.status
  FROM routes r
),
ops_with_params AS (
  SELECT
    o.pattern,
    o.meth,
    o.status,
    json_object(
      'summary', o.summary,
      'parameters', COALESCE((
        SELECT json_group_array(
          json_object(
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
          )
        )
        FROM param_schema ps
        WHERE ps.route_id = o.route_id AND ps.location IN ('path', 'query')
      ), '[]'::JSON),
      'responses', json_object(
        CAST(o.status AS VARCHAR), json_object('description', CASE WHEN o.status=201 THEN 'Created' ELSE 'OK' END, 'content', json_object('application/json', json_object('schema', json_object('type','object')))),
        '422', json_object('description', 'Validation Error', 'content', json_object('application/json', json_object('schema', json_object('type','object'))))
      )
    ) AS op_obj
  FROM ops o
),
pattern_methods AS (
  SELECT
    pattern,
    json_group_object(meth, op_obj) AS methods_map
  FROM ops_with_params
  GROUP BY pattern
),
openapi_body AS (
  SELECT json_object(
    'openapi', '3.0.0',
    'info', json_object('title', 'quackapi', 'version', '0.1.0'),
    'paths', (SELECT json_group_object(pattern, methods_map) FROM pattern_methods)
  ) AS oai
),
docs_html AS (
  SELECT '<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><title>quackapi - Swagger UI</title><link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css"></head><body><div id="swagger-ui"></div><script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js" charset="UTF-8"></script><script>SwaggerUIBundle({url:"/openapi.json",dom_id:"#swagger-ui",presets:[SwaggerUIBundle.presets.apis],layout:"BaseLayout"});</script></body></html>' AS html
),
-- Handler rendering CTEs (A3): only for matched dynamic routes. Use replace + list_reduce (no regex).
-- param_literals built from the same param_values data used by validation (path/query/body all covered).
route_handler AS (
  SELECT r.handler, r.kind
  FROM routes r JOIN best b ON r.route_id = b.route_id
),
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
          ELSE '''' || replace(pv.val_str, '''', '''''') || ''''
        END
      ELSE 'NULL'
    END AS literal
  FROM param_values pv
),
param_list AS (
  SELECT list(struct_pack(name := name, literal := literal)) AS plist FROM param_literals
),
handler_rendered AS (
  SELECT
    ( list_reduce(
        COALESCE(
          list_transform( COALESCE( (SELECT plist FROM param_list), []::STRUCT(name VARCHAR, literal VARCHAR)[] ), lambda p: struct_pack(s := '', name := p.name, literal := p.literal) ),
          []::STRUCT(s VARCHAR, name VARCHAR, literal VARCHAR)[]
        ),
        lambda acc, stp: struct_pack( s := replace(acc.s, '{' || stp.name || '}', stp.literal), name := '', literal := '' ),
        struct_pack( s := (SELECT handler FROM route_handler), name := '', literal := '' )
      )
    ).s AS hsql
  FROM route_handler
)
SELECT
  CASE
    WHEN (SELECT COUNT(*) FROM best) = 0 THEN 404
    WHEN (SELECT COUNT(*) FROM err_rows) > 0 THEN 422
    ELSE (SELECT status FROM best)
  END AS status_code,
  CASE
    WHEN (SELECT COUNT(*) FROM best) = 0 OR (SELECT COUNT(*) FROM err_rows) > 0 THEN 'application/json'
    WHEN (SELECT kind FROM best) = 'html' THEN 'text/html'
    WHEN (SELECT kind FROM best) = 'static' THEN 'application/json'
    WHEN (SELECT kind FROM best) = 'stream' THEN 'text/event-stream'
    ELSE 'application/json'
  END AS content_type,
  CASE
    WHEN (SELECT COUNT(*) FROM best) = 0 THEN
      cast(json_object('detail', 'Not Found') AS VARCHAR)
    WHEN (SELECT COUNT(*) FROM err_rows) > 0 THEN
      cast(json_object('detail', (SELECT detail_arr FROM err_json)) AS VARCHAR)
    WHEN (SELECT kind FROM best) = 'openapi' THEN
      cast((SELECT oai FROM openapi_body) AS VARCHAR)
    WHEN (SELECT kind FROM best) = 'html' THEN
      (SELECT html FROM docs_html)
    WHEN (SELECT kind FROM best) = 'static' THEN
      (SELECT handler FROM route_handler)
    WHEN (SELECT kind FROM best) = 'dynamic' OR (SELECT kind FROM best) = 'stream' THEN
      NULL
    ELSE
      cast(json_object(
        'route', (SELECT route_id FROM best),
        'params', COALESCE((SELECT params_json FROM params_obj), json_object()),
        'message', 'matched (handler execution is a later layer)'
      ) AS VARCHAR)
  END AS body,
  CASE
    WHEN (SELECT COUNT(*) FROM best) = 0 OR (SELECT COUNT(*) FROM err_rows) > 0 THEN NULL
    WHEN (SELECT kind FROM best) = 'openapi' OR (SELECT kind FROM best) = 'html' OR (SELECT kind FROM best) = 'static' THEN NULL
    WHEN (SELECT kind FROM best) = 'dynamic' OR (SELECT kind FROM best) = 'stream' THEN
      (SELECT hsql FROM handler_rendered)
    ELSE NULL
  END AS handler_sql
);

-- GATE self-checks (must produce expected results). Note: dynamic routes now return handler_sql (col3), body=NULL (C layer executes).
-- 200 dynamic show 4 cols: status,ct,NULL,rendered_handler_sql ; 404/422/static show handler_sql=NULL.
SELECT * FROM handle_request('GET','/users/123','{}','');
SELECT * FROM handle_request('GET','/users/abc','{}','');
SELECT * FROM handle_request('GET','/nope/here','{}','');
SELECT * FROM handle_request('GET','/users/7/posts/99','{}','');
-- extended gates (now show handler_sql for dynamic cases)
SELECT * FROM handle_request('GET','/search?q=hi&limit=5','{}','');
SELECT * FROM handle_request('GET','/search?q=hi&limit=abc','{}','');
SELECT * FROM handle_request('GET','/search?q=hi&limit=999','{}','');
SELECT * FROM handle_request('GET','/search?limit=5','{}','');
SELECT * FROM handle_request('POST','/users','{}','{"name":"al","age":30}');
SELECT * FROM handle_request('POST','/users','{}','{"name":"al"}');
SELECT status_code, content_type, substr(body,1,60), handler_sql IS NULL FROM handle_request('GET','/openapi.json','{}','');
SELECT status_code, content_type, substr(body,1,40), handler_sql IS NULL FROM handle_request('GET','/docs','{}','');
SELECT json_extract_string((SELECT body FROM handle_request('GET','/openapi.json','{}','')), '$.openapi') AS v;
