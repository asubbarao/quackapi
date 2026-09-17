-- Benchmark report for one preserved run directory.
--
-- Run this from bench/results/<run-id>. Raw k6 CSV is the latency source;
-- cells.tsv carries request/check validity from the matching summaries.
-- Invalid cells remain visible and are excluded from winner comparisons by
-- the valid column.

CREATE OR REPLACE TEMP TABLE cells AS
SELECT
  export_name,
  stack,
  scenario,
  try_cast(vus AS INTEGER) AS vus,
  try_cast(k6_exit AS INTEGER) AS k6_exit,
  try_cast(measure_requests AS BIGINT) AS measure_requests,
  try_cast(measure_successful AS BIGINT) AS measure_successful,
  try_cast(measure_http_failures AS BIGINT) AS measure_http_failures,
  try_cast(measure_check_failures AS BIGINT) AS measure_check_failures,
  try_cast(valid AS BOOLEAN) AS summary_valid,
  invalid_reason,
  try_cast(all_requests AS BIGINT) AS all_requests,
  try_cast(all_successful AS BIGINT) AS all_successful,
  try_cast(all_http_failures AS BIGINT) AS all_http_failures,
  try_cast(measure_seconds AS DOUBLE) AS measure_seconds
FROM read_csv('cells.tsv', delim := '\t', header := true, auto_detect := true);

CREATE OR REPLACE TEMP TABLE raw_measure AS
SELECT
  split_part(parse_filename(raw.filename), '.', 1) AS export_name,
  try_cast(raw.metric_value AS DOUBLE) AS ms
FROM read_csv('raw/*.csv.gz', filename := true, union_by_name := true) AS raw
WHERE raw.metric_name = 'http_req_duration'
  AND raw.scenario = 'measure'
  AND try_cast(raw.metric_value AS DOUBLE) IS NOT NULL;

CREATE OR REPLACE TEMP TABLE latency AS
SELECT
  export_name,
  count(*) AS sample_count,
  quantile_disc(ms, 0.01) AS p1_ms,
  quantile_disc(ms, 0.50) AS p50_ms,
  quantile_disc(ms, 0.95) AS p95_ms,
  quantile_disc(ms, 0.99) AS p99_ms,
  quantile_disc(ms, 1.00) AS p100_ms
FROM raw_measure
GROUP BY export_name;

CREATE OR REPLACE TEMP TABLE rowcheck AS
SELECT
  stack,
  scenario,
  try_cast(vus AS INTEGER) AS vus,
  try_cast(pg_rows AS BIGINT) AS pg_rows,
  try_cast(k6_ok AS BIGINT) AS k6_ok
FROM read_csv('rowchecks.tsv', delim := '\t', header := true, auto_detect := true);

CREATE OR REPLACE TEMP TABLE benchmark_report AS
SELECT
  c.stack,
  c.scenario,
  c.vus,
  c.measure_requests AS attempted,
  c.measure_successful AS successful,
  c.measure_http_failures AS http_failures,
  c.measure_check_failures AS check_failures,
  round(c.measure_successful / nullif(c.measure_seconds, 0), 3) AS successful_rps,
  l.sample_count,
  round(l.p1_ms, 3) AS p1_ms,
  round(l.p50_ms, 3) AS p50_ms,
  round(l.p95_ms, 3) AS p95_ms,
  round(l.p99_ms, 3) AS p99_ms,
  round(l.p100_ms, 3) AS p100_ms,
  c.all_requests,
  c.all_successful,
  c.all_http_failures,
  r.pg_rows,
  r.k6_ok,
  CASE
    WHEN c.summary_valid
      AND l.sample_count = c.measure_successful
      AND (c.scenario <> 'write' OR r.pg_rows = r.k6_ok)
    THEN true
    ELSE false
  END AS valid,
  concat_ws('; ',
    nullif(c.invalid_reason, ''),
    CASE WHEN l.sample_count IS NULL OR l.sample_count = 0 THEN 'no_latency_samples' END,
    CASE WHEN l.sample_count IS NOT NULL AND l.sample_count <> c.measure_successful
      THEN 'latency_success_count_mismatch' END,
    CASE WHEN c.scenario = 'write' AND (r.pg_rows IS NULL OR r.pg_rows <> r.k6_ok)
      THEN 'write_commit_ack_mismatch' END
  ) AS invalid_reason
FROM cells c
LEFT JOIN latency l
  ON c.export_name = l.export_name
LEFT JOIN rowcheck r
  ON c.stack = r.stack
 AND c.scenario = r.scenario
 AND c.vus = r.vus;

-- Row-oriented output keeps attempted/successful throughput, latency, and the
-- reason an invalid cell was rejected together. Compare only valid=true rows.
SELECT *
FROM benchmark_report
ORDER BY scenario, vus, stack;
