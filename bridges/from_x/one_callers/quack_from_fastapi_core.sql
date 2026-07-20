-- ============================================================================
-- quack_from_fastapi_core.sql
-- Expects placeholders already substituted by the driver:
--   {{APP_GLOB}}  e.g. '/tmp/.../fastapi-realworld/app/**/*.py'
--   {{APP_ROOT}}  e.g. '/tmp/.../fastapi-realworld'
--   {{REPO}}      e.g. 'fastapi-realworld'
--   {{OUT_DIR}}   e.g. '/tmp/quackapi_fromfast/out/fastapi-realworld'
--
-- Produces tables:
--   qf_source_ast, qf_routes_raw, qf_models, qf_prefixes, qf_routes, qf_route_sql
-- And COPY create_sql (base64) to {{OUT_DIR}}/routes.sql.b64
-- ============================================================================

INSTALL sitting_duck FROM community;
LOAD sitting_duck;
INSTALL parser_tools FROM community;
LOAD parser_tools;
LOAD json;

CREATE OR REPLACE MACRO qf_strip_quotes(s) AS (
  CASE
    WHEN s IS NULL THEN NULL
    WHEN starts_with(s, 'r''') OR starts_with(s, 'r"') OR starts_with(s, 'R''') OR starts_with(s, 'R"')
      THEN substring(s, 3, length(s) - 3)
    WHEN length(s) >= 2 AND (
      (s[1] = '''' AND s[length(s)] = '''') OR
      (s[1] = '"' AND s[length(s)] = '"')
    ) THEN substring(s, 2, length(s) - 2)
    ELSE s
  END
);

-- Python annotation → JSON Schema type keyword
CREATE OR REPLACE MACRO qf_py_to_json_type(t) AS (
  CASE
    WHEN lower(coalesce(t, '')) IN ('int', 'integer') THEN 'integer'
    WHEN lower(coalesce(t, '')) IN ('float', 'double', 'decimal', 'number') THEN 'number'
    WHEN lower(coalesce(t, '')) IN ('bool', 'boolean') THEN 'boolean'
    WHEN lower(coalesce(t, '')) IN ('list', 'list[', 'dict', 'dict[', 'set', 'tuple')
      OR starts_with(lower(coalesce(t, '')), 'list[')
      OR starts_with(lower(coalesce(t, '')), 'dict[')
      OR starts_with(lower(coalesce(t, '')), 'optional[list')
      THEN 'array'
    -- Literal["a","b"] field_type is often just "Literal" after split; enums are strings
    WHEN lower(coalesce(t, '')) IN ('str', 'string', 'emailstr', 'httpurl', 'anyurl', 'uuid', 'uuid4',
                                     'datetime', 'date', 'time', 'bytes', 'path', 'secretstr', 'literal')
      OR starts_with(lower(coalesce(t, '')), 'literal')
      OR ends_with(lower(coalesce(t, '')), 'str')
      THEN 'string'
    WHEN lower(coalesce(t, '')) IN ('any', '') THEN 'string'
    ELSE 'object'  -- nested BaseModel name or unknown
  END
);

-- Python annotation → DuckDB cast type for $params
CREATE OR REPLACE MACRO qf_py_to_duck_type(t) AS (
  CASE
    WHEN lower(coalesce(t, '')) IN ('int', 'integer') THEN 'INTEGER'
    WHEN lower(coalesce(t, '')) IN ('float', 'double', 'number', 'decimal') THEN 'DOUBLE'
    WHEN lower(coalesce(t, '')) IN ('bool', 'boolean') THEN 'BOOLEAN'
    WHEN lower(coalesce(t, '')) IN ('list', 'dict') OR starts_with(lower(coalesce(t, '')), 'list')
      OR starts_with(lower(coalesce(t, '')), 'dict') THEN 'JSON'
    ELSE 'VARCHAR'
  END
);

-- Heuristic path-param type from name
CREATE OR REPLACE MACRO qf_param_duck_type(name) AS (
  CASE
    WHEN name ILIKE '%_id' OR name ILIKE '%id' OR name IN ('pk', 'limit', 'offset', 'page', 'size', 'count')
      THEN 'INTEGER'
    ELSE 'VARCHAR'
  END
);

------------------------------------------------------------------------
-- 1. AST ingest (literal glob — sitting_duck constraint)
------------------------------------------------------------------------
CREATE OR REPLACE TABLE qf_source_ast AS
SELECT '{{REPO}}' AS repo, a.*
FROM read_ast('{{APP_GLOB}}', 'python', peek := 'full', ignore_errors := true) a;

CREATE OR REPLACE TABLE qf_ast_stats AS
SELECT count(*) AS nodes, count(DISTINCT file_path) AS files FROM qf_source_ast;

------------------------------------------------------------------------
-- 2. FastAPI routes: @router.get/post/... / @app.get/...
------------------------------------------------------------------------
CREATE OR REPLACE TABLE qf_routes_raw AS
WITH dec AS (
  SELECT
    a.repo, a.file_path, a.node_id AS dec_node_id, a.start_line,
    call_n.node_id AS call_id, lower(call_n.name) AS method_raw, call_n.peek AS call_peek
  FROM qf_source_ast a
  JOIN qf_source_ast dec
    ON dec.file_path = a.file_path AND dec.parent_id = a.node_id AND dec.type = 'decorator'
  JOIN qf_source_ast call_n
    ON call_n.file_path = a.file_path AND call_n.parent_id = dec.node_id AND call_n.type = 'call'
  WHERE a.type = 'decorated_definition'
    AND lower(call_n.name) IN ('get','post','put','delete','patch','options','head','trace','api_route')
),
path_arg AS (
  SELECT d.*, coalesce(qf_strip_quotes(s.name), '') AS path
  FROM dec d
  JOIN qf_source_ast al ON al.file_path = d.file_path AND al.parent_id = d.call_id AND al.type = 'argument_list'
  LEFT JOIN qf_source_ast s
    ON s.file_path = d.file_path AND s.parent_id = al.node_id AND s.type = 'string'
  QUALIFY row_number() OVER (PARTITION BY d.file_path, d.call_id ORDER BY coalesce(s.sibling_index, 0)) = 1
),
handler AS (
  SELECT p.*, fn.name AS handler_name, fn.node_id AS handler_node_id
  FROM path_arg p
  JOIN qf_source_ast fn
    ON fn.file_path = p.file_path AND fn.parent_id = p.dec_node_id AND fn.type = 'function_definition'
)
SELECT
  'fastapi' AS framework,
  CASE WHEN method_raw = 'api_route' THEN 'ANY' ELSE upper(method_raw) END AS method,
  path,
  handler_name,
  handler_node_id,
  file_path AS file,
  start_line,
  repo,
  call_peek AS evidence,
  -- basename without .py for prefix join
  regexp_replace(regexp_extract(file_path, '([^/]+)\.py$', 1), '_', '', 'g') AS file_stem_norm,
  regexp_extract(file_path, '([^/]+)\.py$', 1) AS file_stem
FROM handler;

------------------------------------------------------------------------
-- 3. include_router / app.include_router prefixes
------------------------------------------------------------------------
CREATE OR REPLACE TABLE qf_include_router AS
WITH calls AS (
  SELECT
    a.file_path,
    a.node_id AS call_id,
    a.peek AS call_peek,
    a.start_line
  FROM qf_source_ast a
  WHERE a.type = 'call' AND lower(a.name) = 'include_router'
),
args AS (
  SELECT
    c.*,
    al.node_id AS al_id
  FROM calls c
  JOIN qf_source_ast al ON al.file_path = c.file_path AND al.parent_id = c.call_id AND al.type = 'argument_list'
),
first_arg AS (
  SELECT
    a.file_path, a.call_id, a.call_peek, a.start_line, a.al_id,
    -- first positional: attribute like authentication.router OR identifier api_router
    coalesce(
      (SELECT x.name FROM qf_source_ast x
       WHERE x.file_path = a.file_path AND x.parent_id = a.al_id
         AND x.type IN ('attribute', 'identifier')
       ORDER BY x.sibling_index LIMIT 1),
      ''
    ) AS target_raw
  FROM args a
),
prefix_kw AS (
  SELECT
    f.*,
    coalesce(
      -- keyword prefix= is a direct string child of keyword_argument
      (SELECT qf_strip_quotes(s.name)
       FROM qf_source_ast kw
       JOIN qf_source_ast s
         ON s.file_path = kw.file_path AND s.parent_id = kw.node_id AND s.type = 'string'
       WHERE kw.file_path = f.file_path AND kw.parent_id = f.al_id
         AND kw.type = 'keyword_argument' AND kw.name = 'prefix'
       LIMIT 1),
      -- positional string after first arg
      (SELECT qf_strip_quotes(s.name)
       FROM qf_source_ast s
       WHERE s.file_path = f.file_path AND s.parent_id = f.al_id AND s.type = 'string'
       ORDER BY s.sibling_index LIMIT 1),
      ''
    ) AS prefix
  FROM first_arg f
)
SELECT
  file_path,
  call_id,
  start_line,
  target_raw,
  -- authentication.router → authentication; articles_common.router → articles_common
  regexp_replace(
    regexp_replace(coalesce(target_raw, ''), '\.router$', ''),
    '.*\.',
    ''
  ) AS target_module,
  prefix,
  call_peek AS evidence
FROM prefix_kw;

-- api_prefix string defaults: api_prefix: str = "/api"
CREATE OR REPLACE TABLE qf_api_prefix AS
SELECT coalesce(
  (
    SELECT qf_strip_quotes(s.name)
    FROM qf_source_ast a
    JOIN qf_source_ast t ON t.file_path = a.file_path AND t.parent_id = a.node_id AND t.type = 'type'
    JOIN qf_source_ast eq ON eq.file_path = a.file_path AND eq.parent_id = a.node_id AND eq.type = '='
    JOIN qf_source_ast s ON s.file_path = a.file_path AND s.parent_id = a.node_id AND s.type = 'string'
      AND s.sibling_index > eq.sibling_index
    WHERE a.type = 'assignment' AND a.name = 'api_prefix'
    LIMIT 1
  ),
  '/api'
) AS api_prefix;

-- Map route file_stem → include_router prefix for that module name.
-- Nested package mounts (articles_common under articles.router) get the
-- child prefix; parent empty-prefix includes are not an extra path segment.
CREATE OR REPLACE TABLE qf_file_prefix AS
WITH stems AS (
  SELECT DISTINCT file_stem, file FROM qf_routes_raw
),
matched AS (
  SELECT
    s.file_stem,
    s.file,
    coalesce(
      (SELECT i.prefix FROM qf_include_router i
       WHERE i.target_module = s.file_stem AND coalesce(i.prefix, '') <> ''
       ORDER BY length(i.prefix) DESC LIMIT 1),
      -- articles_resource / articles_common → articles mount
      (SELECT i.prefix FROM qf_include_router i
       WHERE i.target_module = s.file_stem
       LIMIT 1),
      (SELECT i.prefix FROM qf_include_router i
       WHERE starts_with(s.file_stem, i.target_module || '_') AND coalesce(i.prefix, '') <> ''
       ORDER BY length(i.target_module) DESC LIMIT 1),
      ''
    ) AS mount_prefix
  FROM stems s
)
SELECT * FROM matched;

------------------------------------------------------------------------
-- 4. Pydantic / SQLModel models + fields
------------------------------------------------------------------------
CREATE OR REPLACE TABLE qf_class_defs AS
SELECT
  a.repo, a.node_id, a.name AS class_name,
  a.file_path, a.start_line, a.end_line,
  list_transform(
    list_filter(a.parameters, lambda p: p.type = 'extends'),
    lambda p: list_reduce(
      string_split(
        list_reduce(string_split(p.name, '['), lambda acc, x, i: CASE WHEN i = 1 THEN x ELSE acc END),
        '.'
      ),
      lambda acc, x, i: x
    )
  ) AS bases
FROM qf_source_ast a
WHERE a.type = 'class_definition' AND a.name IS NOT NULL;

CREATE OR REPLACE TABLE qf_pydantic_models AS
WITH RECURSIVE reach AS (
  SELECT class_name, file_path, start_line, end_line, node_id, bases, repo,
         0 AS inheritance_depth,
         CASE WHEN list_contains(bases, 'SQLModel') THEN 'sqlmodel' ELSE 'pydantic' END AS model_kind
  FROM qf_class_defs
  WHERE list_contains(bases, 'BaseModel') OR list_contains(bases, 'SQLModel')
     OR list_contains(bases, 'RWModel') OR list_contains(bases, 'RWSchema')
  UNION
  SELECT c.class_name, c.file_path, c.start_line, c.end_line, c.node_id, c.bases, c.repo,
         r.inheritance_depth + 1, r.model_kind
  FROM qf_class_defs c
  JOIN reach r ON list_contains(c.bases, r.class_name)
  WHERE r.inheritance_depth < 16
)
SELECT class_name, file_path, start_line, end_line, node_id, bases, repo,
       min(inheritance_depth) AS inheritance_depth, any_value(model_kind) AS model_kind
FROM reach GROUP BY ALL;

CREATE OR REPLACE TABLE qf_model_fields_own AS
SELECT
  m.model_kind AS framework,
  m.class_name AS model_name,
  a.name AS field_name,
  coalesce(
    nullif(trim(split_part(replace(replace(replace(coalesce(t.peek,''),'Optional[',''),'| None',''),'None |',''),'[',1)), ''),
    'Any'
  ) AS field_type,
  (coalesce(t.peek,'') ILIKE '%Optional[%'
    OR coalesce(t.peek,'') ILIKE '%| None%'
    OR coalesce(t.peek,'') ILIKE '%None |%'
    OR coalesce(rhs.peek,'') = 'None') AS is_optional,
  (eq.node_id IS NOT NULL) AS has_default,
  NOT (eq.node_id IS NOT NULL) AS is_required,
  rhs.peek AS default_expr,
  a.file_path AS file,
  a.start_line AS field_line,
  m.repo,
  t.peek AS declared_annotation
FROM qf_pydantic_models m
JOIN qf_source_ast a
  ON a.file_path = m.file_path AND a.scope.class = m.node_id
 AND a.type = 'assignment' AND a.scope.function IS NULL
LEFT JOIN qf_source_ast t
  ON t.file_path = a.file_path AND t.parent_id = a.node_id AND t.type = 'type'
LEFT JOIN qf_source_ast eq
  ON eq.file_path = a.file_path AND eq.parent_id = a.node_id AND eq.type = '='
LEFT JOIN qf_source_ast rhs
  ON rhs.file_path = a.file_path AND rhs.parent_id = a.node_id
 AND eq.node_id IS NOT NULL AND rhs.sibling_index = eq.sibling_index + 1
WHERE a.name IS NOT NULL AND a.name <> ''
  AND a.name NOT IN ('model_config','Config','Meta')
  AND NOT starts_with(a.name, '_');

-- Flatten inheritance: UserInCreate gets UserInLogin fields, etc.
CREATE OR REPLACE TABLE qf_model_fields AS
WITH RECURSIVE lineage AS (
  SELECT class_name AS model_name, class_name AS ancestor, 0 AS depth
  FROM qf_pydantic_models
  UNION ALL
  SELECT l.model_name, b.base AS ancestor, l.depth + 1
  FROM lineage l
  JOIN qf_pydantic_models m ON m.class_name = l.ancestor
  CROSS JOIN unnest(m.bases) AS b(base)
  WHERE l.depth < 12
    AND b.base IN (SELECT class_name FROM qf_pydantic_models)
)
SELECT
  f.framework,
  l.model_name,
  f.field_name,
  f.field_type,
  f.is_optional,
  f.has_default,
  f.is_required,
  f.default_expr,
  f.file,
  f.field_line,
  f.repo,
  f.declared_annotation
FROM lineage l
JOIN qf_model_fields_own f ON f.model_name = l.ancestor
QUALIFY row_number() OVER (
  PARTITION BY l.model_name, f.field_name
  ORDER BY l.depth  -- own fields (depth 0) win over ancestors
) = 1;

------------------------------------------------------------------------
-- 5. Body model binding from handler signatures (param type ∈ model names)
------------------------------------------------------------------------
CREATE OR REPLACE TABLE qf_handler_body AS
WITH params AS (
  SELECT
    r.file, r.handler_name, r.handler_node_id, r.method, r.path,
    p.name AS param_name,
    -- type node text under the parameter
    coalesce(
      (SELECT t.peek FROM qf_source_ast t
       WHERE t.file_path = p.file_path AND t.parent_id = p.node_id AND t.type = 'type' LIMIT 1),
      (SELECT id.name FROM qf_source_ast id
       WHERE id.file_path = p.file_path AND id.parent_id = p.node_id AND id.type = 'identifier' LIMIT 1)
    ) AS type_raw,
    p.peek AS param_peek
  FROM qf_routes_raw r
  JOIN qf_source_ast pl
    ON pl.file_path = r.file AND pl.parent_id = r.handler_node_id AND pl.type = 'parameters'
  JOIN qf_source_ast p
    ON p.file_path = r.file AND p.parent_id = pl.node_id
   AND p.type IN (
     'typed_parameter', 'default_parameter', 'typed_default_parameter',
     'identifier', 'list_splat_pattern', 'dictionary_splat_pattern'
   )
  WHERE p.name IS NOT NULL
    AND p.name NOT IN ('self', 'cls', 'request', 'response', 'background_tasks')
),
cleaned AS (
  SELECT
    file, handler_name, handler_node_id, method, path, param_name, param_peek,
    -- strip Optional[X], X | None → base type name; take last dotted segment
    regexp_replace(
      regexp_replace(
        regexp_replace(
          regexp_replace(coalesce(type_raw, ''), 'Optional\[([^\]]+)\]', '\1'),
          '\s*\|\s*None', ''
        ),
        'None\s*\|\s*', ''
      ),
      '.*\.',
      ''
    ) AS type_name
  FROM params
)
SELECT
  c.*,
  -- Body embed?
  (c.param_peek ILIKE '%Body(%' OR c.param_peek ILIKE '%Body[%' OR c.param_peek ILIKE '%: % = Body%') AS is_body,
  (c.param_peek ILIKE '%embed=True%' OR c.param_peek ILIKE '%embed = True%') AS body_embed,
  -- alias="user" from Body
  nullif(regexp_extract(c.param_peek, 'alias\s*=\s*["'']([^"'']+)["'']', 1), '') AS body_alias
FROM cleaned c
WHERE c.type_name IS NOT NULL AND c.type_name <> ''
  AND c.type_name IN (SELECT class_name FROM qf_pydantic_models);

------------------------------------------------------------------------
-- 6. JSON Schema per model (flat properties; nested models → type object)
------------------------------------------------------------------------
CREATE OR REPLACE TABLE qf_model_schemas AS
WITH props AS (
  SELECT
    model_name,
    field_name,
    qf_py_to_json_type(field_type) AS json_type,
    is_required,
    is_optional,
    field_type
  FROM qf_model_fields
  WHERE field_name IS NOT NULL
),
agg AS (
  SELECT
    model_name,
    -- properties object as JSON string pieces
    '{' || string_agg(
      '"' || field_name || '":{"type":"' || json_type || '"}',
      ',' ORDER BY field_name
    ) || '}' AS props_json,
    list(field_name ORDER BY field_name) FILTER (WHERE is_required AND NOT is_optional) AS required_fields,
    string_agg(
      CASE WHEN is_required AND NOT is_optional THEN '"' || field_name || '"' ELSE NULL END,
      ',' ORDER BY field_name
    ) AS required_csv,
    -- duck select list for required scalar-ish fields
    string_agg(
      CASE
        WHEN is_required AND NOT is_optional AND json_type IN ('string','integer','number','boolean')
          THEN '$' || field_name || '::' ||
            CASE json_type
              WHEN 'integer' THEN 'INTEGER'
              WHEN 'number' THEN 'DOUBLE'
              WHEN 'boolean' THEN 'BOOLEAN'
              ELSE 'VARCHAR'
            END || ' AS ' || field_name
        ELSE NULL
      END,
      ', ' ORDER BY field_name
    ) AS select_list
  FROM props
  GROUP BY model_name
)
SELECT
  model_name,
  props_json,
  required_fields,
  required_csv,
  select_list,
  '{"type":"object"'
    || CASE WHEN required_csv IS NOT NULL AND required_csv <> ''
         THEN ',"required":[' || required_csv || ']'
         ELSE ''
       END
    || ',"properties":' || props_json || '}' AS schema_json
FROM agg;

------------------------------------------------------------------------
-- 7. Resolved routes: full path + path params + optional body schema
------------------------------------------------------------------------
CREATE OR REPLACE TABLE qf_routes AS
WITH base AS (
  SELECT
    r.*,
    (SELECT api_prefix FROM qf_api_prefix) AS api_prefix,
    coalesce(fp.mount_prefix, '') AS mount_prefix,
    -- compose full path
    regexp_replace(
      regexp_replace(
        coalesce((SELECT api_prefix FROM qf_api_prefix), '')
        || coalesce(fp.mount_prefix, '')
        || CASE WHEN r.path = '' OR r.path IS NULL THEN ''
                WHEN starts_with(r.path, '/') THEN r.path
                ELSE '/' || r.path END,
        '/+', '/', 'g'
      ),
      '/$', ''  -- drop trailing slash except root
    ) AS full_path_raw
  FROM qf_routes_raw r
  LEFT JOIN qf_file_prefix fp ON fp.file = r.file
),
norm AS (
  SELECT
    b.*,
    CASE
      WHEN full_path_raw IS NULL OR full_path_raw = '' THEN '/' || handler_name
      WHEN full_path_raw = '/api' OR full_path_raw = (SELECT api_prefix FROM qf_api_prefix)
        THEN full_path_raw || '/' || handler_name
      ELSE full_path_raw
    END AS full_path
  FROM base b
),
-- path params from {name}; DuckDB regexp_extract_all returns full match — strip braces
path_params AS (
  SELECT
    n.file, n.handler_name, n.start_line,
    regexp_replace(raw, '[{}]', '', 'g') AS param_name
  FROM norm n,
  LATERAL (
    SELECT unnest(
      coalesce(regexp_extract_all(n.full_path, '\{[A-Za-z_][A-Za-z0-9_]*\}'), []::VARCHAR[])
    ) AS raw
  ) u
  WHERE regexp_replace(raw, '[{}]', '', 'g') <> ''
),
param_sql AS (
  SELECT
    file, handler_name, start_line,
    string_agg(
      '  PARAM ' || param_name || ' ' || qf_param_duck_type(param_name),
      chr(10) ORDER BY param_name
    ) AS param_clause,
    string_agg(
      '$' || param_name || '::' || qf_param_duck_type(param_name) || ' AS ' || param_name,
      ', ' ORDER BY param_name
    ) AS param_select
  FROM path_params
  GROUP BY 1, 2, 3
),
body_link AS (
  -- ONLY explicit Body(...) params — never Depends()/response model types on GET
  SELECT *
  FROM qf_handler_body
  WHERE is_body = true
  QUALIFY row_number() OVER (
    PARTITION BY file, handler_name, handler_node_id
    ORDER BY param_name
  ) = 1
)
SELECT
  n.framework,
  n.method,
  n.full_path AS path,
  n.handler_name,
  n.file,
  n.start_line,
  n.repo,
  n.evidence,
  n.path AS rel_path,
  n.mount_prefix,
  n.api_prefix,
  ps.param_clause,
  ps.param_select,
  bl.type_name AS body_model,
  bl.body_embed,
  bl.body_alias,
  bl.is_body,
  ms.schema_json AS body_schema_raw,
  ms.select_list AS body_select,
  -- wrapped schema when Body(embed=True, alias=...)
  CASE
    WHEN bl.type_name IS NULL THEN NULL
    WHEN bl.body_embed AND coalesce(bl.body_alias, '') <> '' THEN
      '{"type":"object","required":["' || bl.body_alias || '"],"properties":{"'
      || bl.body_alias || '":' || ms.schema_json || '}}'
    WHEN bl.body_embed THEN
      '{"type":"object","required":["' || bl.param_name || '"],"properties":{"'
      || bl.param_name || '":' || ms.schema_json || '}}'
    ELSE ms.schema_json
  END AS body_schema,
  bl.param_name AS body_param_name
FROM norm n
LEFT JOIN param_sql ps
  ON ps.file = n.file AND ps.handler_name = n.handler_name AND ps.start_line = n.start_line
LEFT JOIN body_link bl
  ON bl.file = n.file AND bl.handler_name = n.handler_name AND bl.handler_node_id = n.handler_node_id
LEFT JOIN qf_model_schemas ms ON ms.model_name = bl.type_name;

------------------------------------------------------------------------
-- 8. Emit CREATE ROUTE SQL for FastAPI routes
------------------------------------------------------------------------
CREATE OR REPLACE TABLE qf_route_sql AS
SELECT
  'route' AS kind,
  handler_name AS route_name_base,
  method,
  path,
  body_model,
  -- unique route name
  regexp_replace(
    lower(method) || '_' || regexp_replace(handler_name, '[^A-Za-z0-9_]', '_', 'g'),
    '_+', '_', 'g'
  ) AS route_name,
  'CREATE OR REPLACE ROUTE '
    || regexp_replace(lower(method) || '_' || regexp_replace(handler_name, '[^A-Za-z0-9_]', '_', 'g'), '_+', '_', 'g')
    || ' ' || method || ' ''' || path || ''''
    || CASE WHEN method = 'POST' THEN ' STATUS 201' ELSE '' END
    || CASE
         WHEN body_schema IS NOT NULL THEN
           chr(10) || '  BODY SCHEMA ''' || replace(body_schema, '''', '''''') || ''''
         ELSE ''
       END
    || CASE WHEN param_clause IS NOT NULL THEN chr(10) || param_clause ELSE '' END
    || chr(10) || '  AS' || chr(10)
    || 'SELECT '
    || CASE
         -- embed bodies keep nested JSON shape; do not bind nested keys as $params
         WHEN body_embed AND coalesce(param_select, '') <> ''
           THEN param_select || ', ''' || handler_name || ''' AS handler, ''ok'' AS status'
         WHEN body_embed
           THEN '''' || handler_name || ''' AS handler, ''' || coalesce(body_model, '') || ''' AS body_model, ''ok'' AS status'
         WHEN coalesce(param_select, '') <> '' AND coalesce(body_select, '') <> ''
           THEN param_select || ', ' || body_select
         WHEN coalesce(body_select, '') <> '' THEN body_select
         WHEN coalesce(param_select, '') <> '' THEN param_select
         ELSE '''' || handler_name || ''' AS handler, ''' || method || ''' AS method, ''' || path || ''' AS path, ''from_fastapi'' AS source'
       END
    || ';' AS create_sql
FROM qf_routes
WHERE method IN ('GET','POST','PUT','DELETE','PATCH','HEAD','OPTIONS');

------------------------------------------------------------------------
-- 9. Model validation façades (always) — POST /_qf/validate/{Model}
--    Critical for locus (no FastAPI routes) and for body validation demos
------------------------------------------------------------------------
INSERT INTO qf_route_sql
SELECT
  'model_validate' AS kind,
  model_name AS route_name_base,
  'POST' AS method,
  '/_qf/validate/' || model_name AS path,
  model_name AS body_model,
  'validate_' || regexp_replace(model_name, '[^A-Za-z0-9_]', '_', 'g') AS route_name,
  'CREATE OR REPLACE ROUTE validate_'
    || regexp_replace(model_name, '[^A-Za-z0-9_]', '_', 'g')
    || ' POST ''/_qf/validate/' || model_name || ''' STATUS 200'
    || chr(10) || '  BODY SCHEMA ''' || replace(schema_json, '''', '''''') || ''''
    || chr(10) || '  AS' || chr(10)
    || 'SELECT '
    || coalesce(nullif(select_list, ''), '''' || model_name || ''' AS model, ''validated'' AS status')
    || ', ''' || model_name || ''' AS model;' AS create_sql
FROM qf_model_schemas
WHERE model_name IS NOT NULL
  AND NOT starts_with(model_name, '_');

------------------------------------------------------------------------
-- 10. Health + summary
------------------------------------------------------------------------
INSERT INTO qf_route_sql
SELECT
  'health', 'health', 'GET', '/_qf/health', NULL, 'qf_health',
  'CREATE OR REPLACE ROUTE qf_health GET ''/_qf/health'' AS SELECT ''ok'' AS status, ''{{REPO}}'' AS repo;';

CREATE OR REPLACE TABLE qf_summary AS
SELECT
  '{{REPO}}' AS repo,
  '{{APP_ROOT}}' AS app_root,
  (SELECT nodes FROM qf_ast_stats) AS ast_nodes,
  (SELECT files FROM qf_ast_stats) AS ast_files,
  (SELECT count(*) FROM qf_routes_raw) AS routes_found,
  (SELECT count(*) FROM qf_routes) AS routes_resolved,
  (SELECT count(*) FROM qf_route_sql WHERE kind = 'route') AS routes_registered_sql,
  (SELECT count(DISTINCT model_name) FROM qf_model_fields) AS models_found,
  (SELECT count(*) FROM qf_model_fields) AS model_fields,
  (SELECT count(*) FROM qf_route_sql WHERE kind = 'model_validate') AS model_validate_routes,
  (SELECT count(*) FROM qf_route_sql) AS total_create_route_stmts,
  (SELECT count(*) FROM qf_routes WHERE body_schema IS NOT NULL) AS routes_with_body_schema,
  (SELECT count(*) FROM qf_routes WHERE param_clause IS NOT NULL) AS routes_with_path_params,
  (SELECT count(*) FROM qf_include_router) AS include_router_calls;

-- Emit artifacts
COPY (SELECT * FROM qf_summary) TO '{{OUT_DIR}}/summary.json' (FORMAT JSON, ARRAY true);
COPY (
  SELECT method, path, handler_name, body_model, mount_prefix, api_prefix, rel_path, evidence
  FROM qf_routes ORDER BY path, method
) TO '{{OUT_DIR}}/routes_resolved.json' (FORMAT JSON, ARRAY true);
COPY (
  SELECT model_name, field_name, field_type, is_required, is_optional
  FROM qf_model_fields ORDER BY model_name, field_name
) TO '{{OUT_DIR}}/models.json' (FORMAT JSON, ARRAY true);
COPY (
  SELECT kind, route_name, method, path, body_model, create_sql
  FROM qf_route_sql ORDER BY kind, path, method
) TO '{{OUT_DIR}}/route_sql.json' (FORMAT JSON, ARRAY true);
COPY (
  SELECT base64(encode(create_sql)) AS b64
  FROM qf_route_sql
  ORDER BY CASE kind WHEN 'health' THEN 0 WHEN 'route' THEN 1 ELSE 2 END, path, method
) TO '{{OUT_DIR}}/routes.sql.b64' (HEADER false);

SELECT * FROM qf_summary;
SELECT kind, count(*) n FROM qf_route_sql GROUP BY 1 ORDER BY 1;
SELECT method, path, handler_name, body_model FROM qf_routes ORDER BY path, method LIMIT 30;
