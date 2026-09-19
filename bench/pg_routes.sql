-- Three Postgres comparands for ONE quackapi process, so a concurrency sweep
-- compares execution paths rather than machines:
--
--   /attach/:id   DuckDB binds pg.bench_rows and plans the scan itself.
--                 This is the comparand bench/PG_ATTACH_CONCURRENCY.md used.
--   /pgquery/:id  postgres_query() hands the whole statement to Postgres and
--                 streams the result back. DuckDB plans nothing.
--   /attach/:id served with pg_dsn := …  quackapi rewrites the same route SQL
--                 and executes it over its own libpq connection (ToPgSql in
--                 src/quackapi_pg.cpp). postgres_query() is refused there as a
--                 duckdb-only TVF, which is why the native cell reuses /attach.
--
-- postgres_query() addresses an attached catalog by name, so ATTACH is the
-- connection holder for both DuckDB paths. What differs is who plans the scan.

LOAD postgres;

-- SET pg_pool_max_connections (default 24 on a 16-thread host; docs bound it by
-- 4 ≤ cpu_count×1.5 ≤ 32). Pool options apply to databases attached AFTER the
-- SET, so both SETs precede ATTACH.
SET pg_pool_max_connections = 32;
-- SET pg_connection_limit (default 24) — deprecated alias of the pool max, set
-- for older forks that still read it.
SET pg_connection_limit = 32;

-- ATTACH '<dsn>' AS <name> (TYPE postgres, READ_ONLY false, SCHEMA '', SECRET '')
-- The DSN is a literal because ATTACH does not evaluate expressions; the
-- instance it names is created by the recipe at the top of PG_QUERY_2026_09_19.md
-- and is thrown away afterwards. Nothing here reaches a shared database.
ATTACH 'postgresql://bench@127.0.0.1:15432/quackbench' AS pg (TYPE postgres, READ_ONLY);

-- $id::INTEGER is the whole injection story for the concatenated statement: a
-- request that is not an integer fails the cast before any text is built.
CREATE ROUTE pg_attach_item GET '/attach/:id' AS
  SELECT id, name FROM pg.bench_rows WHERE id = $id::INTEGER;

-- postgres_query(database_name, sql) — both arguments required, no defaults.
CREATE ROUTE pg_query_item GET '/pgquery/:id' AS
  SELECT id, name
  FROM postgres_query('pg', 'SELECT id, name FROM bench_rows WHERE id = ' || $id::INTEGER::VARCHAR);
