-- Benchmark report — computed entirely from RAW per-request samples.
--
-- k6 writes one row per metric sample to results/raw/<stack>__<scenario>[__vus<N>].csv.gz
-- (via --out csv). No k6-side aggregation is trusted: the full latency distribution
-- is derived here from the raw http_req_duration samples of the measure stage.
--
-- No percentile number is defined anywhere. cume_dist() writes a percentile onto
-- every row (the full empirical distribution); summary_stats() writes the quartiles
-- and moments. UNPIVOT melts them generically; PIVOT rolls the stack dimension into
-- columns. One declarative CTE chain.

LOAD stats_duck;

CREATE OR REPLACE TEMP TABLE raw AS
SELECT
  -- structural filename parse: <stack>__<scenario>[__vus<N>].csv.gz  (no LIKE)
  split_part(parse_filename(filename), '__', 1)                       AS stack,
  split_part(split_part(parse_filename(filename), '__', 2), '.', 1)   AS scenario,
  coalesce(try_cast(regexp_extract(parse_filename(filename), 'vus(\d+)', 1) AS INTEGER), 0) AS vus,
  metric_value                                                        AS ms
FROM read_csv('results/raw/*.csv.gz', filename := true, union_by_name := true)
WHERE metric_name = 'http_req_duration'
  AND scenario = 'measure';

-- ntile(100) writes the integer percentile (1..100) onto every sample — the full
-- distribution, no percentile number defined. max(ms) per bucket is the latency at
-- that percentile; PIVOT rolls the stack dimension into columns.
CREATE OR REPLACE TEMP TABLE pct AS
SELECT stack, scenario, vus, p, max(ms) AS ms
FROM (
  SELECT stack, scenario, vus, ms,
         ntile(100) OVER (PARTITION BY stack, scenario, vus ORDER BY ms) AS p
  FROM raw
)
GROUP BY ALL;

PIVOT pct
ON stack USING first(round(ms, 3))
GROUP BY scenario, vus, p
ORDER BY scenario, vus, p;
