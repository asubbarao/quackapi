-- The harness guards. Run from the repository root:
--   build/release/duckdb -no-init -f bench/test_harness.sql
--
-- Four things are asserted, and each failing one is a row rather than an early
-- exit, so a single run names everything that is wrong:
--   1. every active harness file exists, is not empty, and is tracked by git
--   2. every benchmark Python file compiles
--   3. the repository holds no shell artifact at all
--   4. no active harness file reaches for a service or binary it does not own
--
-- Guard 1 is weaker than the `bash -n` it replaces, and deliberately so rather
-- than by oversight: DuckDB exposes no parse-without-execute for a script.
-- json_serialize_sql looks like one but only accepts SELECT, so it rejects
-- every file here for the wrong reason. The .sql pipelines are parsed for real
-- when CI runs them; what CI cannot catch on its own is a pipeline that was
-- renamed, emptied, or never committed, which is what this guard holds.
--
-- Guard 3 is the rule this file exists to hold: the pipelines live in .sql and
-- shell out through shellfs, so a .sh reappearing is a regression, not a style
-- preference. Guard 4 previously named pgEdge; the load generator joined it
-- when the benchmark stopped shipping a second language to generate load.

INSTALL shellfs FROM community;
LOAD shellfs;

CREATE OR REPLACE TABLE staging AS
SELECT * FROM read_csv('bash -c ''mkdir -p bench/.tmp; printf "mkdir|%s\n" "$?"'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['gate', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

-- The active harness: what a benchmark run actually parses and executes. The
-- route files beside these (llm_routes.sql, fanout_routes.sql, pg_routes.sql,
-- loopback_client.sql) and the LLM gateway app are deliberately not members --
-- they are separate experiments, they legitimately name the services they talk
-- to, and the base parser cannot read quackapi DDL anyway.
CREATE OR REPLACE TABLE active_harness (path VARCHAR, kind VARCHAR);
INSERT INTO active_harness VALUES
  ('bench/run.sql',                  'sql'),
  ('bench/report.sql',               'sql'),
  ('bench/test_harness.sql',         'sql'),
  ('bench/test_report.sql',          'sql'),
  ('test/conformance/run.sql',       'sql'),
  ('test/ultra/run.sql',             'sql'),
  ('test/ultra/run_pair.sql',        'sql'),
  ('test/ultra/run_extensions.sql',  'sql'),
  ('bench/fetch_upstream.py',        'python'),
  ('bench/migrate.py',               'python'),
  ('bench/fastapi_adapter.py',       'python'),
  ('bench/loadgen.py',               'python'),
  ('bench/conformance.py',           'python'),
  ('bench/crash_client.py',          'python'),
  ('bench/crash_check.py',           'python'),
  ('bench/test_bench_config.py',     'python');

-- git is the authority on what the repository contains: it excludes the
-- submodules and the ignored build output a bare glob would sweep up.
-- read_csv(source, header, names, types, delim, quote, escape, all_varchar,
--   auto_detect, columns, nullstr, skip, sample_size, ignore_errors, filename,
--   hive_partitioning, union_by_name, compression, parallel, ...): only the
--   parameters below are set, the rest keep their DuckDB defaults. One column,
--   so the delimiter is a character a path cannot contain.
CREATE OR REPLACE TABLE repo_files AS
SELECT * FROM read_csv('git ls-files |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['path'], types := ['VARCHAR']);

CREATE OR REPLACE TABLE harness_text AS
SELECT t.filename, t.content, lower(t.content) AS lowered
FROM read_text(['bench/*.sql', 'bench/*.py', 'test/conformance/*.sql', 'test/ultra/*.sql']) t
WHERE t.filename IN (SELECT path FROM active_harness);

-- Guard 2. The one list above drives the compile too, so the guard cannot fall
-- behind the file it is meant to be guarding.
COPY (SELECT path FROM active_harness WHERE kind = 'python' ORDER BY path)
  TO 'bench/.tmp/python_files.csv' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '');

CREATE OR REPLACE TABLE python_compile AS
SELECT * FROM read_csv('bash -c ''
xargs python3 -m py_compile <bench/.tmp/python_files.csv
printf "py_compile|%s\n" "$?"
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['gate', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

-- Guard 4. The banned terms are data, and this file is the one member excluded
-- from the scan: it has to spell them out in order to test for them.
CREATE OR REPLACE TABLE banned_dependency (term VARCHAR, why VARCHAR);
INSERT INTO banned_dependency VALUES
  ('pgedge',       'a service from another repository'),
  (':6432',        'a pgEdge connection pooler port'),
  ('podman start', 'a container the benchmark does not own'),
  ('k6',           'a second language and an external binary in the middle of the benchmark');

CREATE OR REPLACE TABLE dependency_hits AS
SELECT h.filename, b.term, b.why
FROM harness_text h JOIN banned_dependency b ON contains(h.lowered, b.term)
WHERE h.filename <> 'bench/test_harness.sql';

-- The verdict. Each clause keeps the offending rows in a list, so the message
-- names them instead of only counting them.
WITH missing AS (
  SELECT coalesce(array_agg(a.path ORDER BY a.path) FILTER (WHERE t.filename IS NULL OR len(t.content) = 0), []) AS paths
  FROM active_harness a LEFT JOIN harness_text t ON t.filename = a.path
), untracked AS (
  SELECT coalesce(array_agg(a.path ORDER BY a.path) FILTER (WHERE r.path IS NULL), []) AS paths
  FROM active_harness a LEFT JOIN repo_files r ON r.path = a.path
), uncompiled AS (
  SELECT coalesce(array_agg(gate) FILTER (WHERE exit_code <> 0), []) AS gates
  FROM (FROM staging UNION ALL BY NAME FROM python_compile)
), shell_artifacts AS (
  SELECT coalesce(array_agg(path ORDER BY path) FILTER (WHERE ends_with(path, '.sh')), []) AS paths FROM repo_files
), foreign_dependencies AS (
  SELECT coalesce(array_agg(filename || ' mentions ' || term || ' (' || why || ')' ORDER BY filename, term), []) AS hits
  FROM dependency_hits
)
SELECT CASE
  WHEN len(missing.paths) > 0
    THEN error('FAIL: an active harness file is missing or empty: ' || list_aggregate(missing.paths, 'string_agg', ' '))
  WHEN len(untracked.paths) > 0
    THEN error('FAIL: an active harness file is not tracked by git: ' || list_aggregate(untracked.paths, 'string_agg', ' '))
  WHEN len(uncompiled.gates) > 0
    THEN error('FAIL: the benchmark Python does not compile')
  WHEN len(shell_artifacts.paths) > 0
    THEN error('FAIL: the repository holds shell artifacts again: ' || list_aggregate(shell_artifacts.paths, 'string_agg', ' '))
  WHEN len(foreign_dependencies.hits) > 0
    THEN error('FAIL: the harness reaches for something it does not own: ' || list_aggregate(foreign_dependencies.hits, 'string_agg', '; '))
  ELSE 'PASS: the harness is complete and tracked, Python compiles, no shell artifact, no foreign dependency' END AS verdict
FROM missing, untracked, uncompiled, shell_artifacts, foreign_dependencies;
