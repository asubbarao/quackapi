-- The ultra matrix: unit checks, the paired FastAPI/QuackAPI comparison, and
-- the optional-extension probes.
--
-- Run from the repository root:
--   build/release/duckdb -no-init -f test/ultra/run.sql
--
-- FULL=1 adds the repository's HTTP conformance corpus and its scorecard.
-- Controls: DUCKDB, CONFORMANCE_PORT (18774),
-- CONFORMANCE_RESULTS_DIR (test/conformance/results).
--
-- Each stage is a gate row. A failing stage does not stop the ones after it, so
-- one run tells you everything that is broken rather than only the first thing.

INSTALL shellfs FROM community;
LOAD shellfs;

CREATE OR REPLACE TABLE ultra_stages AS
SELECT * FROM read_csv('bash -c ''
set -u
duck=${DUCKDB:-build/release/duckdb}

echo "==> ultra unit checks" >&2
python3 -m unittest discover -s test/ultra -p "test_*.py" >&2
printf "unit|%s\n" "$?"

echo "==> paired FastAPI/QuackAPI matrix" >&2
"$duck" -no-init -f test/ultra/run_pair.sql >&2
printf "paired|%s\n" "$?"

echo "==> version-aware DuckDB extension probes" >&2
"$duck" -no-init -f test/ultra/run_extensions.sql >&2
printf "extensions|%s\n" "$?"

if [ "${FULL:-0}" = 1 ]; then
  echo "==> full HTTP conformance corpus" >&2
  PORT=${CONFORMANCE_PORT:-18774} RESULTS_DIR=${CONFORMANCE_RESULTS_DIR:-test/conformance/results} \
    "$duck" -no-init -f test/conformance/run.sql >&2
  printf "conformance|%s\n" "$?"
  python3 test/conformance/render_scorecard.py >&2
  printf "scorecard|%s\n" "$?"
  echo "==> full SQL/C++/benchmark gates are run by the repository CI and can be repeated with the commands in docs/FASTAPI_ULTRA_MATRIX.md" >&2
fi
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['stage', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

WITH recorded AS (
  SELECT coalesce(array_agg(stage ORDER BY stage) FILTER (WHERE exit_code <> 0), []) AS failed,
         coalesce(array_agg(stage), []) AS stages
  FROM ultra_stages
)
SELECT CASE
  WHEN len(stages) = 0 THEN error('FAIL: no ultra stage reported')
  WHEN len(failed) > 0 THEN error('FAIL: these ultra stages failed: ' || list_aggregate(failed, 'string_agg', ' '))
  ELSE 'PASS: every ultra stage held' END AS verdict
FROM recorded;
