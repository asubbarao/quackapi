-- ============================================================================
-- compose_pg.sql — RECEIPT 12: postgres_scanner, the PostgREST claim.
-- ATTACH a LIVE Postgres database and serve REST endpoints over its tables in
-- two route inserts. PostgREST is an entire product; here it is `LOAD postgres`
-- plus route data — and the same validation/422/OpenAPI pipeline applies.
--
-- Separate file from compose.sql because it REQUIRES a local Postgres
-- (dbname=quackapi_demo, see COMPOSABILITY.md for the 3-line setup).
-- Load order: framework.sql -> compose.sql -> compose_pg.sql.
-- ============================================================================
INSTALL postgres; LOAD postgres;
ATTACH IF NOT EXISTS 'dbname=quackapi_demo host=localhost' AS demo_pg (TYPE postgres, READ_ONLY);

DELETE FROM param_schema WHERE starts_with(route_id, 'composepg_');
DELETE FROM routes WHERE starts_with(route_id, 'composepg_');

-- GET /pg/products — list rows straight off the live Postgres table.
INSERT INTO routes SELECT * FROM register_route(
  'composepg_list', 'GET', '/pg/products',
  'SELECT coalesce(json_group_array(to_json(p)), ''[]'') AS body FROM demo_pg.public.products p',
  'dynamic', 'List products from a LIVE attached Postgres (postgres ext)', 200);

-- GET /pg/products/{id} — single row, int-validated path param (422 on garbage).
INSERT INTO routes SELECT * FROM register_route(
  'composepg_one', 'GET', '/pg/products/{id}',
  'SELECT to_json(p) AS body FROM demo_pg.public.products p WHERE p.id = {id}',
  'dynamic', 'Fetch one product from live Postgres by id', 200);

INSERT INTO param_schema (route_id, name, location, type, required, constraint_json)
SELECT 'composepg_one', 'id', 'path', 'int', true, NULL;
