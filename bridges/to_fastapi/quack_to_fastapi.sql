-- what: the FastAPI pieces for a quackapi route, rendered from what the route already serves:
--   a Pydantic DTO from quack_to_fastapi_columns (route_columns.sql), the FastAPI route with the same method,
--   path and parameters, the route's SQL for that route to run, and a typed TypeScript client.
--   Each <piece>.tera in templates_dir is one file; templates/fastapi is plain FastAPI + duckdb, and a
--   project's own templates (its auth, its database attach, its client) use the same context.
-- run on: the quackapi server, after route_columns.sql for the same route. Set wanted_route.
-- context: op, pascal, camel, method, path, row_class, response_type, empty, body_shape, handler,
--   dto_stdlib, dto_classes, route_stdlib, params, param_values, query_import, ts_interfaces, ts_params,
--   ts_url, ts_response_type, plus every key of wanted_route.extras.
CREATE OR REPLACE TABLE quack_to_fastapi_context AS
WITH wanted_route AS (
    SELECT 'example_orders' AS route_name,
           'http://127.0.0.1:9495/customers/00000000-0000-0000-0000-00000000000a/orders?since=2026-01-01' AS sample_url,
           MAP {}::MAP(VARCHAR, VARCHAR) AS catalog_renames, '{}'::JSON AS extras,
           '/path/to/quackapi/bridges/to_fastapi' AS bridge_dir,
           '/path/to/quackapi/bridges/to_fastapi/templates/fastapi' AS templates_dir,
           '/path/to/quackapi/bridges/to_fastapi/example/app' AS out_dir
), route_meta AS (
    SELECT w.*, lower(r.method) AS method, r.envelope,
           list_reduce(map_entries(w.catalog_renames), lambda h, e: replace(h, concat(e.key, '.'), concat(e.value, '.')), r.handler) AS handler,
           map_keys(urlpattern_exec(urlpattern(r.pattern), w.sample_url).groups) AS path_names, r.pattern,
           concat(extract_schema(w.sample_url), '://', extract_host(w.sample_url), ':', extract_port(w.sample_url)) AS server,
           list_aggregate(list_transform(string_split(w.route_name, '_'), lambda x: concat(upper(x[1]), x[2:])), 'string_agg', '') AS pascal
    FROM quackapi_routes() AS r
    JOIN wanted_route AS w ON w.route_name = r.name
), served_params AS (
    SELECT m.route_name, unnest(receipt.rows) AS param
    FROM (SELECT route_name, array_agg(selfdispatch(concat('SELECT * FROM read_json(',
              quote_literal(concat('curl -s ', server, '/openapi.json | jq -c -f ', bridge_dir, '/openapi_params.jq |')),
              ', format = ''newline_delimited'')'))) AS receipts
          FROM route_meta GROUP BY ALL) AS m
    CROSS JOIN UNNEST(m.receipts) AS t(receipt)
), typed_params AS (
    SELECT route_name, param.position::INTEGER AS position, param.name::VARCHAR AS name, param.place::VARCHAR AS place,
           coalesce(param.required::BOOLEAN, false) AS required,
           CASE param.type::VARCHAR WHEN 'integer' THEN 'int' WHEN 'number' THEN 'float' WHEN 'boolean' THEN 'bool'
                ELSE CASE param.format::VARCHAR WHEN 'uuid' THEN 'uuid.UUID' WHEN 'date' THEN 'date'
                                                WHEN 'date-time' THEN 'datetime' ELSE 'str' END END AS py_type,
           CASE param.type::VARCHAR WHEN 'integer' THEN 'number' WHEN 'number' THEN 'number' WHEN 'boolean' THEN 'boolean'
                ELSE 'string' END AS ts_type
    FROM served_params
    WHERE param.op::VARCHAR = route_name
), param_texts AS (
    SELECT m.route_name,
           CASE WHEN bool_or(p.place = 'query') THEN ', Query' ELSE '' END AS query_import,
           concat_ws($nl$
$nl$, CASE WHEN bool_or(p.py_type = 'uuid.UUID') THEN 'import uuid' END,
                 'from datetime import ' || string_agg(DISTINCT p.py_type, ', ' ORDER BY p.py_type) FILTER (WHERE p.py_type IN ('date', 'datetime'))) AS route_stdlib,
           string_agg(CASE WHEN p.place = 'path' THEN concat(p.name, ': ', p.py_type)
                           WHEN p.required THEN concat(p.name, ': ', p.py_type, ' = Query()')
                           ELSE concat(p.name, ': ', p.py_type, ' | None = Query(default=None)') END, ', '
                      ORDER BY p.place = 'path' DESC, p.position) AS params,
           string_agg(concat('"', p.name, '": ', p.name), ', ' ORDER BY p.position) AS param_values,
           string_agg(concat(p.name, CASE WHEN p.required THEN '' ELSE '?' END, ': ', p.ts_type), ', '
                      ORDER BY p.place = 'path' DESC, p.position) AS ts_params,
           concat(list_reduce(m.path_names, lambda u, k: replace(u, concat(':', k), concat('${encodeURIComponent(', k, ')}')), m.pattern),
                  '?${new URLSearchParams(Object.entries({ ' || string_agg(p.name, ', ' ORDER BY p.position) FILTER (WHERE p.place = 'query')
                  || ' }).filter(([, value]) => value != null).map(([key, value]) => [key, String(value)]))}') AS ts_url
    FROM route_meta AS m
    LEFT JOIN typed_params AS p USING (route_name)
    GROUP BY m.route_name, m.path_names, m.pattern
), type_rules AS (
    SELECT * FROM (VALUES ('VARCHAR', 'str', 'string'), ('UUID', 'uuid.UUID', 'string'), ('DATE', 'date', 'string'),
        ('TIME', 'time', 'string'), ('TIMESTAMP', 'datetime', 'string'), ('BOOLEAN', 'bool', 'boolean'),
        ('TINYINT', 'int', 'number'), ('SMALLINT', 'int', 'number'), ('INTEGER', 'int', 'number'), ('BIGINT', 'int', 'number'),
        ('HUGEINT', 'int', 'number'), ('UTINYINT', 'int', 'number'), ('USMALLINT', 'int', 'number'), ('UINTEGER', 'int', 'number'),
        ('UBIGINT', 'int', 'number'), ('FLOAT', 'float', 'number'), ('DOUBLE', 'float', 'number'),
        ('DECIMAL(', 'Decimal', 'string'), ('BLOB', 'bytes', 'string'), ('JSON', 'Any', 'unknown'), ('VARIANT', 'Any', 'unknown'),
        ('MAP(', 'dict[str, Any]', 'Record<string, unknown>')) AS rule(prefix, py_type, ts_type)
), scalar_types AS (
    SELECT c.route_name, c.parent, c.field, arg_max(r.py_type, len(r.prefix)) AS py_type, arg_max(r.ts_type, len(r.prefix)) AS ts_type
    FROM quack_to_fastapi_columns AS c
    JOIN type_rules AS r ON starts_with(c.element_type, r.prefix)
    GROUP BY ALL
), typed_fields AS (
    SELECT c.route_name, c.parent, c.position, c.field, c.required, c.is_list,
           CASE WHEN c.parent = '' THEN 0 ELSE len(string_split(c.parent, '.')) END AS depth,
           concat(m.pascal, CASE WHEN c.parent = '' THEN 'Row' ELSE list_aggregate(list_transform(
               string_split(replace(c.parent, '.', '_'), '_'), lambda x: concat(upper(x[1]), x[2:])), 'string_agg', '') END) AS owner_class,
           CASE WHEN c.is_struct THEN concat(m.pascal, list_aggregate(list_transform(
                    string_split(replace(concat_ws('.', nullif(c.parent, ''), c.field), '.', '_'), '_'),
                    lambda x: concat(upper(x[1]), x[2:])), 'string_agg', '')) END AS child_class,
           list_transform(c.enum_values, lambda v: concat('"', v, '"')) AS quoted_values, s.py_type, s.ts_type
    FROM quack_to_fastapi_columns AS c
    JOIN route_meta AS m ON m.route_name = c.route_name
    LEFT JOIN scalar_types AS s ON s.route_name = c.route_name AND s.parent = c.parent AND s.field = c.field
), field_types AS (
    SELECT *,
           replace(CASE WHEN is_list THEN 'list[{}]' ELSE '{}' END, '{}', coalesce(child_class,
               'Literal[' || array_to_string(quoted_values, ', ') || ']', py_type, 'Any')) AS py,
           replace(CASE WHEN is_list THEN '{}[]' ELSE '{}' END, '{}', coalesce(child_class,
               CASE WHEN is_list THEN '(' || array_to_string(quoted_values, ' | ') || ')' ELSE array_to_string(quoted_values, ' | ') END,
               ts_type, 'unknown')) AS ts
    FROM typed_fields
), class_texts AS (
    SELECT route_name, owner_class, depth, list(py) AS py_types,
           concat('class ', owner_class, '(BaseModel):', $nl$
$nl$, string_agg(concat('    ', field, ': ', py, CASE WHEN required THEN '' ELSE ' | None = None' END), $nl$
$nl$ ORDER BY position)) AS py_class,
           concat('export interface ', owner_class, ' {', $nl$
$nl$, string_agg(concat('  ', field, CASE WHEN required THEN ': ' ELSE '?: ' END, ts, CASE WHEN required THEN '' ELSE ' | null' END), $nl$
$nl$ ORDER BY position), $nl$
}$nl$) AS ts_interface
    FROM field_types
    GROUP BY route_name, owner_class, depth
), dto_texts AS (
    SELECT route_name,
           string_agg(py_class, $nl$


$nl$ ORDER BY depth DESC, owner_class) AS dto_classes,
           string_agg(ts_interface, $nl$

$nl$ ORDER BY depth DESC, owner_class) AS ts_interfaces,
           flatten(list(py_types)) AS all_py_types
    FROM class_texts
    GROUP BY route_name
), dto_imports AS (
    SELECT route_name, dto_classes, ts_interfaces,
           concat_ws($nl$
$nl$, CASE WHEN list_contains(tokens, 'uuid.UUID') THEN 'import uuid' END,
                 'from datetime import ' || nullif(array_to_string(list_filter(['date', 'datetime', 'time'], lambda n: list_contains(tokens, n)), ', '), ''),
                 CASE WHEN list_contains(tokens, 'Decimal') THEN 'from decimal import Decimal' END,
                 'from typing import ' || nullif(array_to_string(list_filter(['Any', 'Literal'], lambda n: list_contains(tokens, n)), ', '), '')) AS dto_stdlib
    FROM (SELECT *, string_split(translate(array_to_string(all_py_types, ' '), '[], |', '     '), ' ') AS tokens FROM dto_texts)
)
SELECT m.route_name, m.pascal, m.templates_dir, m.out_dir, json_merge_patch({
    op: m.route_name, pascal: m.pascal, camel: concat(lower(m.pascal[1]), m.pascal[2:]), method: m.method,
    path: list_reduce(m.path_names, lambda u, k: replace(u, concat(':', k), concat('{', k, '}')), m.pattern),
    row_class: concat(m.pascal, 'Row'),
    response_type: CASE WHEN m.envelope = 'object' THEN concat(m.pascal, 'Row') ELSE concat('list[', m.pascal, 'Row]') END,
    ts_response_type: CASE WHEN m.envelope = 'object' THEN concat(m.pascal, 'Row') ELSE concat(m.pascal, 'Row[]') END,
    empty: CASE WHEN m.envelope = 'object' THEN 'null' ELSE '[]' END,
    body_shape: CASE WHEN m.envelope = 'object' THEN 'any_value(route_rows)' ELSE 'coalesce(list(route_rows), [])' END,
    handler: trim(m.handler), dto_stdlib: d.dto_stdlib, dto_classes: d.dto_classes, ts_interfaces: d.ts_interfaces,
    route_stdlib: coalesce(t.route_stdlib, ''), params: coalesce(t.params, ''), param_values: coalesce(t.param_values, ''),
    query_import: t.query_import, ts_params: coalesce(t.ts_params, ''), ts_url: t.ts_url
}::JSON, m.extras) AS context
FROM route_meta AS m
JOIN dto_imports AS d USING (route_name)
JOIN param_texts AS t USING (route_name);

-- Every template rendered with the route's context, one file each.
CREATE OR REPLACE TABLE quack_to_fastapi_pieces AS
WITH templates AS (
    SELECT c.route_name, parse_filename(t.file::VARCHAR, true) AS piece, t.content::VARCHAR AS template
    FROM (SELECT route_name, array_agg(selfdispatch(concat('SELECT filename AS file, content FROM read_text(',
              quote_literal(concat(templates_dir, '/*.tera')), ')'))) AS receipts
          FROM quack_to_fastapi_context GROUP BY ALL) AS c
    CROSS JOIN UNNEST(c.receipts) AS r(receipt)
    CROSS JOIN UNNEST(receipt.rows) AS f(t)
)
SELECT c.route_name, t.piece,
       concat(c.out_dir, '/', CASE t.piece WHEN 'api.ts' THEN concat(c.pascal, 'Api.ts')
                                           WHEN 'route.sql' THEN concat(c.route_name, '.sql')
                                           ELSE concat(c.route_name, '_', t.piece) END) AS path,
       tera_render(t.template, c.context, autoescape := false) AS text
FROM quack_to_fastapi_context AS c
JOIN templates AS t USING (route_name);

-- Each piece to its file, then the Python through ruff's formatter.
WITH copy_statements AS (
    SELECT path, concat('COPY (SELECT encode(text) FROM quack_to_fastapi_pieces WHERE path = ', quote_literal(path),
                        ') TO ', quote_literal(path), ' (FORMAT blob)') AS executable_sql
    FROM quack_to_fastapi_pieces
), written AS (
    SELECT path, array_agg(selfdispatch(executable_sql)) AS receipts FROM copy_statements GROUP BY path
)
SELECT path, receipt.status FROM written CROSS JOIN UNNEST(receipts) AS t(receipt) ORDER BY path;

SELECT * FROM read_lines($cmd$uvx ruff format --quiet $(ls /path/to/quackapi/bridges/to_fastapi/example/app/*.py) 2>&1; echo "ruff format exit $?" |$cmd$);
