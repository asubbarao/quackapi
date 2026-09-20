-- Version-aware probes of the optional DuckDB extensions quackapi can sit
-- beside. This is a probe, not a gate: when no binary has a matching set
-- installed it records a SKIP and succeeds, exactly as the shell driver did.
--
-- Run from the repository root:
--   build/release/duckdb -no-init -f test/ultra/run_extensions.sql
--
-- Controls: DUCKDB (a candidate binary to try first),
-- EXTENSION_RESULTS_DIR (test/ultra/results/extensions-<utc timestamp>).
--
-- The shell driver this replaces found the extension directory by taking the
-- version out of `--version` with awk and then rebuilding
-- ~/.duckdb/extensions/<version>/<platform> by hand, trying four platform
-- names. That is a guess about a path layout. Asking the binary to LOAD the
-- two extensions is the same question asked directly, and it cannot be wrong
-- about which build is version-matched.

INSTALL shellfs FROM community;
LOAD shellfs;

CREATE OR REPLACE TABLE staging AS
SELECT * FROM read_csv('bash -c ''mkdir -p test/ultra/.tmp; printf "mkdir|%s\n" "$?"'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['gate', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

-- Every candidate is kept with the answer it gave, so a SKIP can be explained.
CREATE OR REPLACE TABLE extension_candidates AS
SELECT * FROM read_csv('bash -c ''
set -u
for binary in "${DUCKDB:-}" build/release/duckdb build/debug/duckdb "$(command -v duckdb 2>/dev/null || true)"; do
  [ -n "$binary" ] || continue
  [ -x "$binary" ] || continue
  version=$("$binary" -no-init --version 2>/dev/null | tail -1)
  "$binary" -no-init -unsigned -c "LOAD httpfs_timeout_retry; LOAD cache_prewarm;" >/dev/null 2>&1
  printf "%s|%s|%s\n" "$binary" "$version" "$?"
done
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['binary_path', 'version', 'required_exit_code'],
  types := ['VARCHAR', 'VARCHAR', 'INTEGER']);

CREATE OR REPLACE TABLE extension_chosen AS
SELECT binary_path, version FROM extension_candidates
WHERE required_exit_code = 0
LIMIT 1;

COPY extension_chosen TO 'test/ultra/.tmp/chosen.csv' (FORMAT csv, HEADER false, DELIMITER '|', QUOTE '', ESCAPE '');

-- Which of the optional set this binary can actually load. Each answer is a
-- row; nothing is dropped for being unavailable.
COPY (SELECT unnest(['cache_httpfs', 'http_stats', 'finetype', 'query_condition_cache', 'table_guard']) AS name)
  TO 'test/ultra/.tmp/optional.csv' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '');

-- Every optional extension gets a row whether or not it could be probed. An
-- empty stream is a read_csv error rather than zero rows, so the SKIP path --
-- no version-matched binary at all -- has to answer with rows too, and
-- `probed` is what distinguishes "did not load" from "was never asked".
CREATE OR REPLACE TABLE extension_optional AS
SELECT * FROM read_csv('bash -c ''
set -u
while read -r name; do
  if [ -s test/ultra/.tmp/chosen.csv ]; then
    while IFS="|" read -r binary version; do
      "$binary" -no-init -unsigned -c "LOAD $name;" >/dev/null 2>&1
      printf "%s|true|%s\n" "$name" "$?"
    done <test/ultra/.tmp/chosen.csv
  else
    printf "%s|false|\n" "$name"
  fi
done <test/ultra/.tmp/optional.csv
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['name', 'probed', 'exit_code'], types := ['VARCHAR', 'BOOLEAN', 'INTEGER']);

-- The probe session, built from what loaded. One statement per row, so no
-- string is interpolated into a literal anywhere.
COPY (
  SELECT 'LOAD httpfs_timeout_retry;' AS statement
  UNION ALL SELECT 'LOAD cache_prewarm;'
  UNION ALL SELECT 'LOAD ' || name || ';' FROM extension_optional WHERE probed AND exit_code = 0
  UNION ALL SELECT 'CREATE TABLE extension_prewarm_probe AS SELECT range AS id FROM range(100);'
  UNION ALL SELECT 'SELECT extension_name, loaded, installed FROM duckdb_extensions() '
                || 'WHERE extension_name IN (''httpfs_timeout_retry'', ''cache_prewarm'', ''cache_httpfs'', '
                || '''http_stats'', ''finetype'', ''query_condition_cache'', ''table_guard'') ORDER BY extension_name;'
  UNION ALL SELECT 'SET httpfs_timeout_file_operation_ms = 1000;'
  UNION ALL SELECT 'SET httpfs_retries_file_operation = 2;'
  UNION ALL SELECT 'SELECT name, value FROM duckdb_settings() '
                || 'WHERE name IN (''httpfs_timeout_file_operation_ms'', ''httpfs_retries_file_operation'', '
                || '''cache_httpfs_type'', ''cache_httpfs_max_in_mem_cache_block_count'') ORDER BY name;'
  UNION ALL SELECT 'SELECT function_name, function_type, array_to_string(parameter_types, '','') AS parameter_types '
                || 'FROM duckdb_functions() WHERE function_name IN (''prewarm'', ''prewarm_remote'', ''finetype'', '
                || '''finetype_detail'', ''finetype_validate'', ''condition_cache_info'', ''condition_cache_stats'', '
                || '''table_guard_status'') ORDER BY function_name, parameter_types;'
  UNION ALL SELECT 'SELECT finetype_version() AS finetype_version, finetype(''42'') AS inferred_type, finetype_detail(''42'') AS detail;'
            FROM extension_optional WHERE name = 'finetype' AND probed AND exit_code = 0
  UNION ALL SELECT 'SELECT try(prewarm(''extension_prewarm_probe'', ''read'')) AS prewarm_local_probe;'
) TO 'test/ultra/.tmp/probe.sql' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '');

COPY (
  SELECT unnest(['duckdb', 'version', 'status']) AS name,
         unnest([coalesce((SELECT binary_path FROM extension_chosen), ''),
                 coalesce((SELECT version FROM extension_chosen), ''),
                 CASE WHEN exists (FROM extension_chosen) THEN 'RUN' ELSE 'SKIP' END]) AS value
) TO 'test/ultra/.tmp/manifest.csv' (FORMAT csv, HEADER false, DELIMITER '|', QUOTE '', ESCAPE '');

CREATE OR REPLACE TABLE extension_probe AS
SELECT * FROM read_csv('bash -c ''
set -u
OUT=${EXTENSION_RESULTS_DIR:-test/ultra/results/extensions-$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$OUT"
cp test/ultra/.tmp/manifest.csv "$OUT/manifest.csv"
if [ ! -s test/ultra/.tmp/chosen.csv ]; then
  echo "SKIP: no binary could load httpfs_timeout_retry + cache_prewarm; wrote $OUT/manifest.csv" >&2
  printf "probe|0\n"; exit 0
fi
while IFS="|" read -r binary version; do
  cp test/ultra/.tmp/probe.sql "$OUT/probe.sql"
  case "$binary" in
    */build/debug/duckdb) ASAN_OPTIONS=${ASAN_OPTIONS:-detect_container_overflow=0} "$binary" -no-init -unsigned -json -f "$OUT/probe.sql" >"$OUT/probe.json" 2>&1 ;;
    *) "$binary" -no-init -unsigned -json -f "$OUT/probe.sql" >"$OUT/probe.json" 2>&1 ;;
  esac
  printf "probe|%s\n" "$?"
  echo "optional extension probes written to $OUT" >&2
done <test/ultra/.tmp/chosen.csv
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['gate', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

WITH recorded AS (
  SELECT coalesce(array_agg(gate ORDER BY gate) FILTER (WHERE exit_code <> 0), []) AS failed
  FROM (FROM staging UNION ALL BY NAME FROM extension_probe)
)
SELECT CASE WHEN len(failed) > 0
              THEN error('FAIL: ' || list_aggregate(failed, 'string_agg', ' '))
            ELSE 'PASS: optional extension probes recorded' END AS verdict
FROM recorded;
