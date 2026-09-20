-- The benchmark report. Reads the three raw files from the working directory,
-- so bench/run.sql runs it inside the run directory and bench/test_report.sql
-- runs it inside a fixture directory.
--
-- Nothing here collapses a set of values into a single number on its own. Every
-- cell keeps its per-trial values as a list, and the scalar beside the list is
-- derived from that list -- the list is the fact, the scalar is a view of it.

-- read_json(source, format, columns, records, maximum_object_size, lines,
--   ignore_errors, hive_partitioning, union_by_name, filename, compression,
--   dateformat, timestampformat, auto_detect, sample_size, maximum_depth,
--   field_appearance_threshold, map_inference_threshold,
--   convert_strings_to_integers): only `format` is set, every other parameter
--   keeps its DuckDB default. The whole document is read and the inferred
--   columns are selected by name.
CREATE OR REPLACE TEMP TABLE measurements AS
SELECT * FROM read_json('measurements.jsonl', format = 'newline_delimited');

CREATE OR REPLACE TEMP TABLE conformance AS
SELECT * FROM read_json('conformance.jsonl', format = 'newline_delimited');

CREATE OR REPLACE TEMP TABLE crash AS
SELECT * FROM read_json('crash.jsonl', format = 'newline_delimited');

-- Every measured cell, unaggregated. This is the evidence the summary below is
-- a view of; a reader who distrusts a summary row reads the matching rows here.
SELECT stack, trial, concurrency, attempted, successful, successful_rps,
       p50_ms, p99_ms, max_ms, contract_failures, shed, timeouts, resets, eofs,
       other_no_response
FROM measurements
ORDER BY concurrency, stack, trial;

-- The summary. Each list holds one value per trial; the scalar next to it is
-- derived from that same list.
SELECT
  stack,
  concurrency,
  array_agg(DISTINCT trial ORDER BY trial)                       AS trials,
  len(trials)                                                    AS trial_count,
  array_agg(attempted ORDER BY trial)                            AS attempted_per_trial,
  list_sum(attempted_per_trial)                                  AS total_attempted,
  array_agg(successful ORDER BY trial)                           AS successful_per_trial,
  list_sum(successful_per_trial)                                 AS total_successful,
  round(100.0 * total_successful / nullif(total_attempted, 0), 3) AS success_pct,
  array_agg(successful_rps ORDER BY trial)                       AS successful_rps_per_trial,
  round(list_aggregate(successful_rps_per_trial, 'median'), 1)   AS median_successful_rps,
  array_agg(p50_ms ORDER BY trial)                               AS p50_ms_per_trial,
  round(list_aggregate(p50_ms_per_trial, 'median'), 3)           AS median_p50_ms,
  array_agg(p99_ms ORDER BY trial)                               AS p99_ms_per_trial,
  round(list_aggregate(p99_ms_per_trial, 'median'), 3)           AS median_p99_ms,
  array_agg(max_ms ORDER BY trial)                               AS max_ms_per_trial,
  round(list_sort(max_ms_per_trial)[-1], 3)                      AS worst_max_ms,
  array_agg(contract_failures ORDER BY trial)                    AS contract_failures_per_trial,
  list_sum(contract_failures_per_trial)                          AS total_contract_failures,
  array_agg(shed ORDER BY trial)                                 AS shed_per_trial,
  list_sum(shed_per_trial)                                       AS total_shed,
  array_agg(timeouts ORDER BY trial)                             AS timeouts_per_trial,
  list_sum(timeouts_per_trial)                                   AS total_timeouts,
  array_agg(resets ORDER BY trial)                               AS resets_per_trial,
  list_sum(resets_per_trial)                                     AS total_resets,
  array_agg(eofs ORDER BY trial)                                 AS eofs_per_trial,
  list_sum(eofs_per_trial)                                       AS total_eofs,
  array_agg(other_no_response ORDER BY trial)                    AS other_no_response_per_trial,
  list_sum(other_no_response_per_trial)                          AS total_other_no_response
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
