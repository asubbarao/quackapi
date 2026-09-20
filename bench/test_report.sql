-- bench/report.sql, driven against a fixture.
--
-- Run from the repository root:
--   build/release/duckdb -no-init -f bench/test_report.sql
--
-- The report is where a bad number becomes a published claim, so the facts that
-- must never be summarised away are asserted here: the worst latency any trial
-- saw, the per-trial lists the summary is derived from, the error counters kept
-- apart from one another, and the crash survivors.

INSTALL shellfs FROM community;
LOAD shellfs;

CREATE OR REPLACE TABLE fixture_dir AS
SELECT * FROM read_csv('bash -c ''
rm -rf bench/.tmp/report_fixture
mkdir -p bench/.tmp/report_fixture
printf "%s|%s\n" mkdir "$?"
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['gate', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

-- Three trials per stack, and one quackapi trial whose max latency is three
-- orders of magnitude above its p99. The outlier sits in the middle of the
-- trials on purpose: a summary that reaches for the median instead of the worst
-- value reports 15ms and hides a ten-second response.
COPY (
  SELECT unnest(['quackapi', 'quackapi', 'quackapi', 'fastapi', 'fastapi', 'fastapi']) AS stack,
         unnest([1, 2, 3, 1, 2, 3])            AS trial,
         unnest([32, 32, 32, 32, 32, 32])      AS concurrency,
         unnest([96, 96, 96, 90, 90, 90])      AS attempted,
         unnest([92, 96, 96, 90, 90, 90])      AS successful,
         unnest([100, 101, 99, 90, 91, 89])    AS successful_rps,
         unnest([1, 1, 1, 2, 2, 2])            AS p50_ms,
         unnest([9, 9, 9, 10, 10, 10])         AS p99_ms,
         unnest([12, 10028, 15, 12, 13, 14])   AS max_ms,
         unnest([0, 0, 0, 0, 0, 0])            AS contract_failures,
         unnest([6, 0, 0, 0, 0, 0])            AS shed,
         unnest([0, 0, 0, 0, 0, 0])            AS timeouts,
         unnest([4, 0, 0, 0, 0, 0])            AS resets,
         unnest([0, 0, 0, 0, 0, 0])            AS eofs,
         unnest([0, 0, 0, 0, 0, 0])            AS other_no_response
) TO 'bench/.tmp/report_fixture/measurements.jsonl' (FORMAT json);

COPY (
  SELECT unnest(['quackapi', 'fastapi']) AS stack,
         unnest([8, 9])                  AS passed,
         unnest([9, 9])                  AS total
) TO 'bench/.tmp/report_fixture/conformance.jsonl' (FORMAT json);

COPY (
  SELECT unnest(['quackapi', 'fastapi']) AS stack,
         unnest([64, 64])                AS attempted,
         unnest([64, 64])                AS acknowledged,
         unnest([64, 0])                 AS survived
) TO 'bench/.tmp/report_fixture/crash.jsonl' (FORMAT json);

CREATE OR REPLACE TABLE report_run AS
SELECT * FROM read_csv('bash -c ''
set -u
report=$PWD/bench/report.sql
duckdb=${DUCKDB_BIN:-build/release/duckdb}
case "$duckdb" in /*) ;; *) duckdb=$PWD/$duckdb ;; esac
( cd bench/.tmp/report_fixture && "$duckdb" -no-init -csv -f "$report" >report.csv 2>&1 )
printf "report|%s\n" "$?"
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['gate', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

-- The whole document, kept as lines. An assertion is a row that has to find at
-- least one of them.
CREATE OR REPLACE TABLE report_lines AS
SELECT unnest(string_split(content, chr(10))) AS line
FROM read_text('bench/.tmp/report_fixture/report.csv');

CREATE OR REPLACE TABLE report_assertions (assertion VARCHAR, needle VARCHAR);
-- The first needle is the summary's latency list immediately followed by the
-- scalar derived from it, which is the only place those two characters meet.
-- A bare "10028" would also match the unaggregated rows above the summary and
-- would let a summary that reported the median pass.
INSERT INTO report_assertions VALUES
  ('the worst latency any trial saw is the summary value, not the median', '"[12, 10028, 15]",10028,'),
  ('the per-trial latency list the summary derives from is kept',          '"[12, 10028, 15]"'),
  ('every error counter is still its own column',                          'total_contract_failures'),
  ('shed is not folded into the other error counters',                     'total_shed'),
  ('the unaggregated per-trial rows are printed',                          'stack,trial,concurrency,attempted'),
  ('the crash row keeps attempted, acknowledged and survivors',            'quackapi,64,64,64,0'),
  ('the volatile control still shows every job lost',                      'fastapi,64,64,0,64');

-- array_agg keeps NULLs, so the unmatched side of the LEFT JOIN would otherwise
-- aggregate to [NULL] and read as a hit.
WITH found AS (
  SELECT a.assertion,
         coalesce(array_agg(l.line) FILTER (WHERE l.line IS NOT NULL), []) AS matching_lines
  FROM report_assertions a LEFT JOIN report_lines l ON contains(l.line, a.needle)
  GROUP BY a.assertion
), broken AS (
  SELECT coalesce(array_agg(gate), []) AS gates
  FROM (FROM fixture_dir UNION ALL BY NAME FROM report_run)
  WHERE exit_code <> 0
), missing AS (
  SELECT coalesce(array_agg(assertion ORDER BY assertion), []) AS assertions
  FROM found WHERE len(matching_lines) = 0
)
SELECT CASE WHEN len(gates) > 0
              THEN error('FAIL: the report could not be produced: ' || list_aggregate(gates, 'string_agg', ' '))
            WHEN len(assertions) > 0
              THEN error('FAIL: the report dropped a fact it must keep: ' || list_aggregate(assertions, 'string_agg', '; '))
            ELSE 'PASS: the report preserves worst latency, per-trial evidence and crash survivors' END AS verdict
FROM broken, missing;
