-- what: every field a quackapi route returns, typed by DuckDB itself, for quack_to_fastapi.sql:
--   DESCRIBE of the handler with its parameters bound to NULL, then DESCRIBE of each struct, a nesting
--   level per round; ENUM values from enum_range; required where every column of that name and type in the
--   tables the handler reads is NOT NULL (information_schema), unless SUMMARIZE finds it null in the live
--   route at the sample URLs. Path parameters come from the route's pattern (urlpattern), query parameters
--   from the server's /openapi.json, the server from the sample URLs (netquack).
-- run on: the quackapi server that serves the route, with urlpattern, netquack and tera loaded and jq on PATH.
--   Set wanted_route, then run quack_to_fastapi.sql.
CREATE OR REPLACE TABLE quack_to_fastapi_columns AS
WITH wanted_route AS (
    SELECT 'example_orders' AS route_name, [current_database()] AS source_catalogs,
           ['http://127.0.0.1:9495/customers/00000000-0000-0000-0000-00000000000a/orders?since=2026-01-01',
            'http://127.0.0.1:9495/customers/00000000-0000-0000-0000-00000000000b/orders?since=2026-01-01'] AS sample_urls,
           '/path/to/quackapi/bridges/to_fastapi' AS bridge_dir
), route_meta AS (
    SELECT w.*, r.handler, r.pattern,
           concat(extract_schema(w.sample_urls[1]), '://', extract_host(w.sample_urls[1]), ':', extract_port(w.sample_urls[1])) AS server
    FROM quackapi_routes() AS r
    JOIN wanted_route AS w ON w.route_name = r.name
), served_ops AS (
    SELECT m.route_name, unnest(receipt.rows) AS op
    FROM (SELECT route_name, array_agg(selfdispatch(concat('SELECT * FROM read_json(',
              quote_literal(concat('curl -s ', server, '/openapi.json | jq -c -f ', bridge_dir, '/openapi_ops.jq |')),
              ', format = ''newline_delimited'')'))) AS receipts
          FROM route_meta GROUP BY ALL) AS m
    CROSS JOIN UNNEST(m.receipts) AS t(receipt)
), route_params AS (
    SELECT route_name, op.param_names::VARCHAR[] AS params
    FROM served_ops
    WHERE op.op::VARCHAR = route_name
), described AS (
    -- level 1: the handler itself; levels 2 and 3: the structs inside it
    SELECT m.route_name, '' AS parent, unnest(receipt.rows) AS field, generate_subscripts(receipt.rows, 1) AS position
    FROM (SELECT m.route_name, array_agg(selfdispatch(concat('DESCRIBE ', list_reduce(coalesce(p.params, []),
              lambda bound, name: replace(bound, concat('$', name), 'NULL'), m.handler)))) AS receipts
          FROM route_meta AS m LEFT JOIN route_params AS p USING (route_name) GROUP BY ALL) AS m
    CROSS JOIN UNNEST(m.receipts) AS t(receipt)
), level_2 AS (
    SELECT s.route_name, s.parent, unnest(receipt.rows) AS field, generate_subscripts(receipt.rows, 1) AS position
    FROM (SELECT route_name, field.column_name::VARCHAR AS parent,
                 array_agg(selfdispatch(concat('DESCRIBE SELECT unnest(NULL::', replace(field.column_type::VARCHAR, ')[]', ')'), ')'))) AS receipts
          FROM described WHERE starts_with(field.column_type::VARCHAR, 'STRUCT(') GROUP BY ALL) AS s
    CROSS JOIN UNNEST(s.receipts) AS t(receipt)
), level_3 AS (
    SELECT s.route_name, s.parent, unnest(receipt.rows) AS field, generate_subscripts(receipt.rows, 1) AS position
    FROM (SELECT route_name, concat(parent, '.', field.column_name::VARCHAR) AS parent,
                 array_agg(selfdispatch(concat('DESCRIBE SELECT unnest(NULL::', replace(field.column_type::VARCHAR, ')[]', ')'), ')'))) AS receipts
          FROM level_2 WHERE starts_with(field.column_type::VARCHAR, 'STRUCT(') GROUP BY ALL) AS s
    CROSS JOIN UNNEST(s.receipts) AS t(receipt)
), fields AS (
    SELECT route_name, parent, position, field.column_name::VARCHAR AS field, field.column_type::VARCHAR AS column_type,
           CASE WHEN ends_with(field.column_type::VARCHAR, '[]') THEN (field.column_type::VARCHAR)[1:-3]
                ELSE field.column_type::VARCHAR END AS element_type
    FROM (SELECT * FROM described UNION ALL SELECT * FROM level_2 UNION ALL SELECT * FROM level_3)
), enum_values AS (
    SELECT element_type, receipt.rows[1].enum_values::VARCHAR[] AS enum_values
    FROM (SELECT element_type, array_agg(selfdispatch(concat('SELECT enum_range(NULL::', element_type, ') AS enum_values'))) AS receipts
          FROM (SELECT DISTINCT element_type FROM fields WHERE starts_with(element_type, 'ENUM(')) GROUP BY ALL)
    CROSS JOIN UNNEST(receipts) AS t(receipt)
), source_columns AS (
    SELECT f.route_name, f.parent, f.field, bool_and(c.is_nullable = 'NO') AS not_null_in_source
    FROM fields AS f
    JOIN route_meta AS m USING (route_name)
    JOIN information_schema.columns AS c
      ON list_contains(m.source_catalogs, c.table_catalog) AND c.column_name = f.field AND c.data_type = f.element_type
     AND contains(m.handler, concat(c.table_schema, '.', c.table_name))
    GROUP BY ALL
), observed_nulls AS (
    SELECT s.route_name, unnest(receipt.rows) AS summary
    FROM (SELECT route_name, array_agg(selfdispatch(concat('SUMMARIZE SELECT * FROM read_json(',
              list_transform(sample_urls, lambda u: concat('curl -s "', u, '" |'))::VARCHAR, ')'))) AS receipts
          FROM wanted_route GROUP BY ALL) AS s
    CROSS JOIN UNNEST(s.receipts) AS t(receipt)
)
SELECT f.route_name, f.parent, f.position, f.field, f.column_type, f.element_type,
       ends_with(f.column_type, '[]') AS is_list, starts_with(f.element_type, 'STRUCT(') AS is_struct,
       e.enum_values, coalesce(s.not_null_in_source, false) AS not_null_in_source,
       o.summary.null_percentage::DOUBLE AS null_percentage,
       coalesce(s.not_null_in_source, false) AND coalesce(o.summary.null_percentage::DOUBLE, 0) = 0 AS required
FROM fields AS f
LEFT JOIN enum_values AS e USING (element_type)
LEFT JOIN source_columns AS s USING (route_name, parent, field)
LEFT JOIN observed_nulls AS o
       ON o.route_name = f.route_name AND f.parent = '' AND o.summary.column_name::VARCHAR = f.field
ORDER BY f.route_name, f.parent, f.position;
