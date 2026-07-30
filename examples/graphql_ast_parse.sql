-- GraphQL document → (table, columns[]) as ordinary SQL via sitting_duck.
--
-- Thesis: parse is not a special C++ mystery. Tree-sitter AST is a table;
-- mapping fields to SELECT targets is a CTE. quackapi's hot path keeps a
-- zero-dep C++ thin parser for serve; this recipe proves the dual-surface
-- claim that "it resolves to a query" can start at the *parse* layer too.
--
-- // hatch: HTTP body is a string; read_ast wants a path. Write a .graphql
-- // file (or use a path the client already has). Could be Duck temp when
-- // optional AST path is wired into the server.
--
-- Requires: INSTALL/LOAD sitting_duck (community).

INSTALL sitting_duck FROM community;
LOAD sitting_duck;

-- Sample document (same shape as GraphQL v0).
-- // hatch: string body → path; serve hot path uses C++ and never needs this.
COPY (SELECT 'query { users { id name } posts { title } }')
TO '/tmp/quackapi_gql_demo.graphql' (HEADER false, QUOTE '');

COPY (SELECT '{ secrets { id } }')
TO '/tmp/quackapi_gql_bare.graphql' (HEADER false, QUOTE '');

-- AST → root fields + leaf columns (v0: one level of selection under each root).
WITH nodes AS (
	SELECT node_id, parent_id, type, name
	FROM read_ast('/tmp/quackapi_gql_demo.graphql')
),
root_fields AS (
	SELECT f.node_id, f.name AS table_name
	FROM nodes f
	JOIN nodes sel ON sel.node_id = f.parent_id AND sel.type = 'selection'
	JOIN nodes ss ON ss.node_id = sel.parent_id AND ss.type = 'selection_set'
	JOIN nodes op ON op.node_id = ss.parent_id AND op.type = 'operation_definition'
	WHERE f.type = 'field'
),
cols AS (
	SELECT r.table_name, c.name AS column_name, c.node_id
	FROM root_fields r
	JOIN nodes ss ON ss.parent_id = r.node_id AND ss.type = 'selection_set'
	JOIN nodes sel ON sel.parent_id = ss.node_id AND sel.type = 'selection'
	JOIN nodes c ON c.parent_id = sel.node_id AND c.type = 'field'
),
field_map AS (
	SELECT table_name, list(column_name ORDER BY node_id) AS columns
	FROM cols
	GROUP BY table_name
)
SELECT
	table_name,
	columns,
	-- Same SQL shape quackapi GraphQL v0 executes per root field:
	format(
		'SELECT {} FROM {} LIMIT 100',
		list_aggregate(list_transform(columns, lambda c: '"' || c || '"'), 'string_agg', ', '),
		'"' || table_name || '"'
	) AS resolved_sql
FROM field_map
ORDER BY table_name;

-- Bare `{ secrets { id } }` uses the same CTE (operation_definition still present).
WITH nodes AS (
	SELECT node_id, parent_id, type, name
	FROM read_ast('/tmp/quackapi_gql_bare.graphql')
),
root_fields AS (
	SELECT f.node_id, f.name AS table_name
	FROM nodes f
	JOIN nodes sel ON sel.node_id = f.parent_id AND sel.type = 'selection'
	JOIN nodes ss ON ss.node_id = sel.parent_id AND ss.type = 'selection_set'
	JOIN nodes op ON op.node_id = ss.parent_id AND op.type = 'operation_definition'
	WHERE f.type = 'field'
),
cols AS (
	SELECT r.table_name, c.name AS column_name, c.node_id
	FROM root_fields r
	JOIN nodes ss ON ss.parent_id = r.node_id AND ss.type = 'selection_set'
	JOIN nodes sel ON sel.parent_id = ss.node_id AND sel.type = 'selection'
	JOIN nodes c ON c.parent_id = sel.node_id AND c.type = 'field'
)
SELECT table_name, list(column_name ORDER BY node_id) AS columns
FROM cols
GROUP BY table_name;
