-- Boot the quackapi server. Run from the repo root:
--   duckdb < launch_server.sql
-- Override the DB path by editing the serve_brain(...) call below
-- (':memory:' for ephemeral, or any path you like). Default is repo-local.
.read framework.sql
.read serve_brain.sql
SELECT serve_brain(18099, 'quackapi.db') AS listen;
SELECT block_forever(0);
