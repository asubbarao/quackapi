CREATE OR REPLACE TEMP TABLE measurements AS
SELECT * FROM read_json_auto('measurements.jsonl', format = 'newline_delimited');

CREATE OR REPLACE TEMP TABLE conformance AS
SELECT * FROM read_json_auto('conformance.jsonl', format = 'newline_delimited');

CREATE OR REPLACE TEMP TABLE crash AS
SELECT * FROM read_json_auto('crash.jsonl', format = 'newline_delimited');

SELECT
  stack,
  concurrency,
  count(*) AS trials,
  sum(attempted) AS attempted,
  sum(successful) AS successful,
  round(100.0 * sum(successful) / nullif(sum(attempted), 0), 3) AS success_pct,
  round(median(successful_rps), 1) AS median_successful_rps,
  round(median(p50_ms), 3) AS median_p50_ms,
  round(median(p99_ms), 3) AS median_p99_ms,
  round(max(max_ms), 3) AS worst_max_ms,
  sum(contract_failures) AS contract_failures,
  sum(timeouts) AS timeouts,
  sum(resets) AS resets,
  sum(eofs) AS eofs,
  sum(other_no_response) AS other_no_response
FROM measurements
GROUP BY stack, concurrency
ORDER BY concurrency, stack;

SELECT stack, passed, total, total - passed AS failed
FROM conformance
ORDER BY stack;

SELECT stack, attempted, acknowledged, survived,
       acknowledged - survived AS acknowledged_but_not_surviving
FROM crash
ORDER BY stack;
