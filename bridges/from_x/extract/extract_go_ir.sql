-- extract_go_ir.sql — sitting_duck language='go' (verified)
-- IR: /tmp/quackapi_corpus/ir_go_routes.parquet + ir_go_models.parquet

LOAD sitting_duck;

CREATE OR REPLACE MACRO is_route_method(m) AS (
  upper(m) IN ('GET','POST','PUT','PATCH','DELETE','HEAD','OPTIONS')
  OR m IN ('Get','Post','Put','Patch','Delete','Head','Options')
);
CREATE OR REPLACE MACRO normalize_method(m) AS (
  CASE
    WHEN m IN ('Get','GET') THEN 'GET'
    WHEN m IN ('Post','POST') THEN 'POST'
    WHEN m IN ('Put','PUT') THEN 'PUT'
    WHEN m IN ('Patch','PATCH') THEN 'PATCH'
    WHEN m IN ('Delete','DELETE') THEN 'DELETE'
    WHEN m IN ('Head','HEAD') THEN 'HEAD'
    WHEN m IN ('Options','OPTIONS') THEN 'OPTIONS'
    ELSE upper(m)
  END
);
CREATE OR REPLACE MACRO strip_quotes(s) AS trim(both '"' FROM coalesce(s, ''));
CREATE OR REPLACE MACRO join_paths(a, b) AS (
  CASE
    WHEN coalesce(b,'') IN ('', '/') THEN
      CASE WHEN right(coalesce(a,''),1)='/' THEN left(a, length(a)-1) ELSE coalesce(nullif(a,''), '/') END
    WHEN coalesce(a,'') IN ('','/') THEN
      CASE WHEN left(b,1)='/' THEN b ELSE '/' || b END
    ELSE
      (CASE WHEN right(a,1)='/' THEN left(a, length(a)-1) ELSE a END)
      || (CASE WHEN left(b,1)='/' THEN b ELSE '/' || b END)
  END
);

-- Build AST slices separately then union with explicit columns
CREATE OR REPLACE TEMP TABLE ast_all AS
SELECT
  node_id, type, name, semantic_type, start_line, end_line, parent_id, depth,
  children_count, peek, file_path, language,
  'gin'::VARCHAR AS framework,
  'golang-gin-realworld-example-app'::VARCHAR AS source_repo
FROM read_ast('/tmp/quackapi_corpus/go/golang-gin-realworld-example-app/**/*.go', 'go', peek := 'full');

INSERT INTO ast_all
SELECT node_id, type, name, semantic_type, start_line, end_line, parent_id, depth,
       children_count, peek, file_path, language, 'echo', 'golang-echo-realworld-example-app'
FROM read_ast('/tmp/quackapi_corpus/go/golang-echo-realworld-example-app/**/*.go', 'go', peek := 'full');

INSERT INTO ast_all
SELECT node_id, type, name, semantic_type, start_line, end_line, parent_id, depth,
       children_count, peek, file_path, language, 'gin', 'gin-examples'
FROM read_ast('/tmp/quackapi_corpus/go/gin-examples/**/*.go', 'go', peek := 'full');

INSERT INTO ast_all
SELECT node_id, type, name, semantic_type, start_line, end_line, parent_id, depth,
       children_count, peek, file_path, language, 'echo', 'echo-cookbook'
FROM read_ast('/tmp/quackapi_corpus/go/echo-cookbook/**/*.go', 'go', peek := 'full');

INSERT INTO ast_all
SELECT node_id, type, name, semantic_type, start_line, end_line, parent_id, depth,
       children_count, peek, file_path, language, 'chi', 'chi-repo'
FROM read_ast('/tmp/quackapi_corpus/go/chi-repo/_examples/**/*.go', 'go', peek := 'full');

INSERT INTO ast_all
SELECT node_id, type, name, semantic_type, start_line, end_line, parent_id, depth,
       children_count, peek, file_path, language, 'fiber', 'fiber-recipes'
FROM read_ast('/tmp/quackapi_corpus/go/fiber-recipes/**/main.go', 'go', peek := 'full');

INSERT INTO ast_all
SELECT node_id, type, name, semantic_type, start_line, end_line, parent_id, depth,
       children_count, peek, file_path, language, 'fiber', 'fiber-recipes'
FROM read_ast('/tmp/quackapi_corpus/go/fiber-recipes/**/application.go', 'go', peek := 'full');

INSERT INTO ast_all
SELECT node_id, type, name, semantic_type, start_line, end_line, parent_id, depth,
       children_count, peek, file_path, language, 'fiber', 'fiber-recipes'
FROM read_ast('/tmp/quackapi_corpus/go/fiber-recipes/**/handler/*.go', 'go', peek := 'full');

SELECT source_repo, framework, count(*) AS nodes, count(DISTINCT file_path) AS files
FROM ast_all GROUP BY 1,2 ORDER BY 1;

-- ─── Route calls (join keys = file_path + node_id) ───
CREATE OR REPLACE TEMP TABLE route_calls AS
WITH calls AS (
  SELECT
    file_path,
    node_id AS call_id,
    framework,
    source_repo,
    start_line,
    end_line,
    name AS method_raw,
    peek AS registration
  FROM ast_all
  WHERE type = 'call_expression' AND is_route_method(name)
),
arg_lists AS (
  SELECT c.file_path, c.call_id, al.node_id AS arg_list_id
  FROM calls c
  JOIN ast_all al
    ON al.file_path = c.file_path
   AND al.parent_id = c.call_id
   AND al.type = 'argument_list'
),
arg_children AS (
  SELECT a.file_path, a.call_id, ch.node_id, ch.type, ch.peek, ch.name
  FROM arg_lists a
  JOIN ast_all ch
    ON ch.file_path = a.file_path
   AND ch.parent_id = a.arg_list_id
),
path_args AS (
  SELECT file_path, call_id, arg_min(peek, node_id) AS path_lit
  FROM arg_children
  WHERE type = 'interpreted_string_literal'
  GROUP BY file_path, call_id
),
handlers AS (
  SELECT file_path, call_id,
    arg_min(
      CASE WHEN type = 'func_literal' THEN '<anon_func>' ELSE peek END,
      node_id
    ) AS handler
  FROM arg_children
  WHERE type IN ('identifier','selector_expression','func_literal')
  GROUP BY file_path, call_id
)
SELECT
  c.framework,
  normalize_method(c.method_raw) AS method,
  strip_quotes(p.path_lit) AS raw_path,
  coalesce(h.handler, '<unknown>') AS handler,
  c.file_path AS file,
  c.source_repo,
  c.start_line,
  c.registration,
  c.call_id
FROM calls c
JOIN path_args p ON p.file_path = c.file_path AND p.call_id = c.call_id
LEFT JOIN handlers h ON h.file_path = c.file_path AND h.call_id = c.call_id
WHERE strip_quotes(p.path_lit) = ''
   OR strip_quotes(p.path_lit) LIKE '/%'
   OR strip_quotes(p.path_lit) LIKE '*%'
   OR strip_quotes(p.path_lit) LIKE ':%';

SELECT framework, source_repo, count(*) AS n_routes
FROM route_calls GROUP BY 1,2 ORDER BY 1,2;

CREATE OR REPLACE TEMP TABLE route_with_fn AS
SELECT
  r.*,
  (
    SELECT f.name
    FROM ast_all f
    WHERE f.file_path = r.file
      AND f.type = 'function_declaration'
      AND f.start_line <= r.start_line
      AND f.end_line >= r.start_line
    ORDER BY f.start_line DESC
    LIMIT 1
  ) AS enclosing_fn
FROM route_calls r;

CREATE OR REPLACE TEMP TABLE gin_register_prefix AS
SELECT * FROM (VALUES
  ('UsersRegister', '/api/users'),
  ('UserRegister', '/api/user'),
  ('ProfileRetrieveRegister', '/api/profiles'),
  ('ProfileRegister', '/api/profiles'),
  ('ArticlesAnonymousRegister', '/api/articles'),
  ('ArticlesRegister', '/api/articles'),
  ('TagsAnonymousRegister', '/api/tags')
) t(register_fn, prefix);

CREATE OR REPLACE TEMP TABLE routes_resolved AS
SELECT
  framework,
  method,
  CASE
    WHEN source_repo = 'golang-gin-realworld-example-app'
      AND enclosing_fn IN (SELECT register_fn FROM gin_register_prefix)
      THEN join_paths(
        (SELECT prefix FROM gin_register_prefix g WHERE g.register_fn = enclosing_fn),
        raw_path
      )
    WHEN source_repo = 'golang-echo-realworld-example-app' AND file LIKE '%/handler/routes.go'
      THEN join_paths(
        CASE
          WHEN registration LIKE 'guestUsers.%' THEN '/api/users'
          WHEN registration LIKE 'user.%' THEN '/api/user'
          WHEN registration LIKE 'profiles.%' THEN '/api/profiles'
          WHEN registration LIKE 'articles.%' THEN '/api/articles'
          WHEN registration LIKE 'tags.%' THEN '/api/tags'
          ELSE '/api'
        END,
        raw_path
      )
    WHEN source_repo = 'golang-gin-realworld-example-app' AND registration LIKE 'testAuth.%'
      THEN join_paths('/api/ping', raw_path)
    ELSE raw_path
  END AS path,
  raw_path,
  handler,
  file,
  source_repo,
  start_line,
  enclosing_fn,
  registration,
  CASE
    WHEN source_repo = 'golang-gin-realworld-example-app'
      AND enclosing_fn IN (SELECT register_fn FROM gin_register_prefix)
      THEN (SELECT prefix FROM gin_register_prefix g WHERE g.register_fn = enclosing_fn)
    WHEN source_repo = 'golang-echo-realworld-example-app' AND file LIKE '%/handler/routes.go'
      THEN CASE
        WHEN registration LIKE 'guestUsers.%' THEN '/api/users'
        WHEN registration LIKE 'user.%' THEN '/api/user'
        WHEN registration LIKE 'profiles.%' THEN '/api/profiles'
        WHEN registration LIKE 'articles.%' THEN '/api/articles'
        WHEN registration LIKE 'tags.%' THEN '/api/tags'
        ELSE NULL
      END
    ELSE NULL
  END AS group_prefix
FROM route_with_fn;

CREATE OR REPLACE TABLE ir_go_routes AS
SELECT
  framework,
  method,
  path,
  handler,
  file,
  source_repo,
  start_line::INTEGER AS start_line,
  raw_path,
  group_prefix,
  enclosing_fn,
  registration
FROM routes_resolved
ORDER BY framework, source_repo, path, method;

COPY ir_go_routes TO '/tmp/quackapi_corpus/ir_go_routes.parquet' (FORMAT PARQUET);

SELECT 'ROUTES_BY_FW' AS kind, framework, source_repo, count(*)::BIGINT AS n
FROM ir_go_routes GROUP BY 1,2,3 ORDER BY 2,3;
SELECT 'ROUTES_TOTAL' AS kind, count(*)::BIGINT AS n FROM ir_go_routes;
SELECT method, path, handler, group_prefix, start_line
FROM ir_go_routes
WHERE source_repo = 'golang-gin-realworld-example-app' AND file NOT LIKE '%_test.go'
ORDER BY path, method, start_line;
SELECT method, path, handler, group_prefix, start_line
FROM ir_go_routes
WHERE source_repo = 'golang-echo-realworld-example-app' AND file NOT LIKE '%_test.go'
ORDER BY path, method, start_line;

-- ─── Models ───
CREATE OR REPLACE TEMP TABLE field_tags AS
SELECT
  framework,
  source_repo,
  file_path AS file,
  fd.node_id AS field_node,
  fd.start_line,
  fd.name AS field_name,
  (
    SELECT t.peek FROM ast_all t
    WHERE t.file_path = fd.file_path
      AND t.parent_id = fd.node_id
      AND t.type IN ('type_identifier','pointer_type','slice_type','qualified_type','array_type','map_type')
    ORDER BY t.node_id LIMIT 1
  ) AS field_type,
  (
    SELECT r.peek FROM ast_all r
    WHERE r.file_path = fd.file_path
      AND r.parent_id = fd.node_id
      AND r.type = 'raw_string_literal'
    ORDER BY r.node_id LIMIT 1
  ) AS tag_lit,
  (
    SELECT ts.name FROM ast_all ts
    WHERE ts.file_path = fd.file_path
      AND ts.type = 'type_spec'
      AND ts.start_line <= fd.start_line
      AND ts.end_line >= fd.start_line
    ORDER BY ts.start_line DESC LIMIT 1
  ) AS model_name
FROM ast_all fd
WHERE fd.type = 'field_declaration'
  AND fd.name IS NOT NULL
  AND fd.name <> '';

CREATE OR REPLACE TABLE ir_go_models AS
SELECT
  framework,
  model_name AS model,
  field_name AS field,
  coalesce(field_type, 'unknown') AS type,
  (
    regexp_matches(coalesce(tag_lit,''), 'binding:"[^"]*required')
    OR regexp_matches(coalesce(tag_lit,''), 'validate:"[^"]*required')
  ) AS required,
  nullif(regexp_extract(coalesce(tag_lit,''), 'json:"([^,"]+)', 1), '') AS json_name,
  trim(both '`' FROM coalesce(tag_lit, '')) AS tags,
  CASE
    WHEN regexp_extract(coalesce(tag_lit,''), 'binding:"([^"]+)"', 1) <> ''
      THEN regexp_extract(coalesce(tag_lit,''), 'binding:"([^"]+)"', 1)
    WHEN regexp_extract(coalesce(tag_lit,''), 'validate:"([^"]+)"', 1) <> ''
      THEN regexp_extract(coalesce(tag_lit,''), 'validate:"([^"]+)"', 1)
    ELSE NULL
  END AS validation_tag,
  file,
  source_repo,
  start_line::INTEGER AS start_line
FROM field_tags
WHERE tag_lit IS NOT NULL
  AND (tag_lit LIKE '%binding:%' OR tag_lit LIKE '%validate:%' OR tag_lit LIKE '%json:%')
ORDER BY framework, model, field;

COPY ir_go_models TO '/tmp/quackapi_corpus/ir_go_models.parquet' (FORMAT PARQUET);

SELECT 'MODELS_BY_FW' AS kind, framework, source_repo, count(*)::BIGINT AS n
FROM ir_go_models GROUP BY 1,2,3 ORDER BY 2,3;
SELECT 'MODELS_TOTAL' AS kind, count(*)::BIGINT AS n FROM ir_go_models;
SELECT 'REQUIRED' AS kind, count(*)::BIGINT AS n FROM ir_go_models WHERE required;
SELECT model, field, type, required, json_name, validation_tag
FROM ir_go_models
WHERE required AND source_repo IN ('golang-gin-realworld-example-app','golang-echo-realworld-example-app')
ORDER BY source_repo, model, field;

DESCRIBE ir_go_routes;
DESCRIBE ir_go_models;
