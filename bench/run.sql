-- quackapi vs FastAPI migration benchmark, end to end.
--
-- Run from the repository root, after `make release`:
--   build/release/duckdb -no-init -f bench/run.sql
--
-- Every phase shells out through shellfs and lands one row per gate carrying
-- the program's exit code as a column. Nothing is swallowed and nothing stops
-- the sweep: the last statement turns any nonzero row into error(), so duckdb
-- itself exits nonzero and names every gate that failed.
--
-- The knobs are read from the environment by the prepare phase -- the process
-- that uses them -- and come back as the bench_run row, so the configuration a
-- run used is data rather than prose. bench/.tmp/run.csv is the same row on
-- disk, because each shellfs command is its own process and has to be told
-- where the run directory is:
--   RESULTS_ROOT BENCH_WORK_ROOT BENCH_VENV DUCKDB_BIN QUACKAPI_EXT
--   TRIALS DURATION_SEC WARMUP_SEC CONCURRENCY_LEVELS REQUEST_TIMEOUT_SEC
--   CRASH_JOBS CRASH_DELAY_MS QUACKAPI_PORT FASTAPI_PORT
--   QUACKAPI_WORKER_THREADS QUACKAPI_MAX_PENDING_REQUESTS QUACKAPI_PG_DSN

INSTALL shellfs FROM community;
LOAD shellfs;

-- Phase 1 -- prepare. Verify the build, make the immutable run directory, fetch
-- and checksum the pinned upstream tree, build the venv, generate the routes.
-- read_csv(source, header, names, types, delim, quote, escape, all_varchar,
--   auto_detect, columns, nullstr, skip, sample_size, ignore_errors, filename,
--   hive_partitioning, union_by_name, compression, parallel, new_line,
--   decimal_separator, dateformat, timestampformat, null_padding,
--   max_line_size, normalize_names, store_rejects): only the parameters named
--   below are set, every other one keeps its DuckDB default.
CREATE OR REPLACE TABLE bench_run AS
SELECT * FROM read_csv('bash -c ''
set -u
RESULTS_ROOT=${RESULTS_ROOT:-bench/results}
WORK_ROOT=${BENCH_WORK_ROOT:-bench/.tmp/honest}
VENV=${BENCH_VENV:-bench/.venv}
DUCKDB_BIN=${DUCKDB_BIN:-build/release/duckdb}
QUACKAPI_EXT=${QUACKAPI_EXT:-build/release/extension/quackapi/quackapi.duckdb_extension}
RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)-$$
RESULTS=$RESULTS_ROOT/$RUN_ID
APP=$WORK_ROOT/upstream/docs_src/bigger_applications/app_an_py310
st=0
test -f "$DUCKDB_BIN" || { echo "error: no duckdb binary at $DUCKDB_BIN" >&2; st=1; }
test -f "$QUACKAPI_EXT" || { echo "error: no quackapi extension at $QUACKAPI_EXT" >&2; st=1; }
mkdir -p "$WORK_ROOT" "$RESULTS/raw" bench/.tmp || st=1
: >"$RESULTS/measurements.jsonl"; : >"$RESULTS/conformance.jsonl"; : >"$RESULTS/crash.jsonl"
: >"$RESULTS/fastapi_survivors.txt"
[ $st -eq 0 ] && { python3 bench/fetch_upstream.py --manifest bench/upstream_files.json --destination "$WORK_ROOT/upstream" >/dev/null || st=$?; }
[ $st -eq 0 ] && [ ! -x "$VENV/bin/uvicorn" ] && { python3 -m venv "$VENV" || st=$?; }
[ $st -eq 0 ] && { "$VENV/bin/python" -m pip install -q --disable-pip-version-check -r bench/requirements.txt || st=$?; }
[ $st -eq 0 ] && { "$VENV/bin/python" bench/migrate.py --duckdb "$DUCKDB_BIN" --extension "$QUACKAPI_EXT" --source "$APP" --output-dir "$RESULTS" >/dev/null || st=$?; }
[ $st -eq 0 ] || exit $st
printf "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n" \
  "$RUN_ID" "$RESULTS" "$DUCKDB_BIN" "$QUACKAPI_EXT" "$VENV" "$APP" \
  "${TRIALS:-3}" "${DURATION_SEC:-3}" "${WARMUP_SEC:-0.5}" "${CONCURRENCY_LEVELS:-1 8 32 64 320}" \
  "${REQUEST_TIMEOUT_SEC:-12}" "${CRASH_JOBS:-64}" "${CRASH_DELAY_MS:-3000}" \
  "${QUACKAPI_PORT:-18080}" "${FASTAPI_PORT:-18081}" \
  "${QUACKAPI_WORKER_THREADS:-32}" "${QUACKAPI_MAX_PENDING_REQUESTS:-}" "${QUACKAPI_PG_DSN:-}" \
  | tee bench/.tmp/run.csv
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['run_id', 'results_dir', 'duckdb_bin', 'quackapi_ext', 'venv', 'upstream_app',
            'trials', 'duration_sec', 'warmup_sec', 'concurrency_levels', 'request_timeout_sec',
            'crash_jobs', 'crash_delay_ms', 'quackapi_port', 'fastapi_port',
            'worker_threads', 'max_pending_requests', 'pg_dsn'],
  types := ['VARCHAR', 'VARCHAR', 'VARCHAR', 'VARCHAR', 'VARCHAR', 'VARCHAR',
            'INTEGER', 'DOUBLE', 'DOUBLE', 'VARCHAR', 'DOUBLE',
            'INTEGER', 'INTEGER', 'INTEGER', 'INTEGER',
            'INTEGER', 'VARCHAR', 'VARCHAR']);

-- Prepare cannot print an exit code on the row it failed to print, so an empty
-- read is its failure. The gate exists either way.
CREATE OR REPLACE TABLE bench_prepare AS
SELECT 'prepare' AS gate, CASE WHEN len(array_agg(run_id)) = 1 THEN 0 ELSE 1 END AS exit_code
FROM bench_run;

-- Phase 2 -- the environment this run actually had, as rows, teed into the run
-- directory so the record travels with the measurements.
CREATE OR REPLACE TABLE bench_environment AS
SELECT * FROM read_csv('bash -c ''
while IFS="|" read -r run_id results duckdb ext venv app trials duration warmup levels timeout jobs delay qport fport workers pending dsn; do
  {
    printf "run_id|%s\n" "$run_id"
    printf "git_head|%s\n" "$(git rev-parse HEAD)"
    printf "upstream_repo|%s\n" https://github.com/fastapi/fastapi
    printf "upstream_commit|%s\n" 50113da16fec53b66b80d75e80a89296de4fa5a5
    printf "duckdb|%s\n" "$($duckdb -no-init -version)"
    printf "python|%s\n" "$(python3 --version)"
    printf "trials|%s\n" "$trials"
    printf "duration_sec|%s\n" "$duration"
    printf "warmup_sec|%s\n" "$warmup"
    printf "concurrency_levels|%s\n" "$levels"
    printf "worker_threads|%s\n" "$workers"
    printf "max_pending_requests|%s\n" "${pending:-derived}"
    "$venv/bin/python" -m pip freeze | while read -r pinned; do printf "pip|%s\n" "$pinned"; done
  } | tee "$results/environment.csv"
done <bench/.tmp/run.csv
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['name', 'value'], types := ['VARCHAR', 'VARCHAR']);

-- Phase 3 -- the serve statement, composed from the recorded configuration
-- instead of by string substitution in a shell. worker_threads is the only HTTP
-- dial: the pending queue follows it unless QUACKAPI_MAX_PENDING_REQUESTS pins
-- one, which is what a cell wanting the two budgets apart deliberately does.
-- An empty pg_dsn keeps the DuckDB handler path; a DSN selects native libpq.
COPY (
  SELECT 'SELECT * FROM quackapi_serve(' || quackapi_port
      || ', host := ''127.0.0.1'', access_log := false, enable_logging := false'
      || ', worker_threads := ' || worker_threads
      || coalesce(', max_pending_requests := ' || nullif(max_pending_requests, ''), '')
      || coalesce(', pg_dsn := ''' || nullif(pg_dsn, '') || '''', '')
      || ', block := true);' AS statement
  FROM bench_run
) TO 'bench/.tmp/serve_quackapi.sql' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '');

-- Phase 4 -- the sweep plan. Odd trials run quackapi first, even trials run
-- FastAPI first, so a drifting machine cannot favour one stack. Every run-level
-- constant rides on the row: the walker below resolves no defaults of its own,
-- so a knob is decided in exactly one place.
CREATE OR REPLACE TABLE bench_sessions AS
SELECT row_number() OVER (ORDER BY trial, CASE WHEN trial % 2 = 1 THEN side ELSE 3 - side END) AS seq,
       stack, trial,
       CASE WHEN stack = 'quackapi' THEN quackapi_port ELSE fastapi_port END AS port,
       'http://127.0.0.1:' || CASE WHEN stack = 'quackapi' THEN quackapi_port ELSE fastapi_port END
         || '/users/rick?token=jessica' AS url,
       '{"username":"rick"}' AS expect_json,
       concurrency_levels, trial = 1 AS run_conformance,
       duration_sec, warmup_sec, request_timeout_sec,
       results_dir, duckdb_bin, venv, upstream_app
FROM bench_run,
     (SELECT unnest(generate_series(1, (SELECT trials FROM bench_run))) AS trial),
     (SELECT unnest(['quackapi', 'fastapi']) AS stack, unnest([1, 2]) AS side)
ORDER BY seq;

COPY bench_sessions TO 'bench/.tmp/sessions.csv' (FORMAT csv, HEADER false, DELIMITER '|', QUOTE '', ESCAPE '');

-- Phase 5 -- walk the plan. One server per session, killed before the next one
-- starts. A failing program is recorded and the walk continues, so the whole
-- matrix is still measured when one cell violates its contract.
CREATE OR REPLACE TABLE bench_sweep AS
SELECT * FROM read_csv('bash -c ''
set -u
while IFS="|" read -r seq stack trial port url expect levels conf duration warmup timeout results duckdb venv app; do
  log=$results/${stack}_trial${trial}_server.log
  if [ "$stack" = quackapi ]; then
    db=$results/quackapi_trial${trial}.duckdb
    "$duckdb" -no-init -unsigned "$db" -c "INSTALL curl_httpfs FROM community; INSTALL httpfs_timeout_retry FROM community;" >"$log" 2>&1
    "$duckdb" -no-init -unsigned "$db" -f "$results/generated_routes.sql" -f bench/.tmp/serve_quackapi.sql >>"$log" 2>&1 &
  else
    UPSTREAM_SOURCE_PARENT=$(dirname "$app") FASTAPI_SURVIVORS=$results/fastapi_survivors.txt \
      "$venv/bin/uvicorn" fastapi_adapter:app --app-dir bench --host 127.0.0.1 --port "$port" --workers 1 --no-access-log >"$log" 2>&1 &
  fi
  pid=$!
  ready=0
  for _ in $(seq 1 160); do
    kill -0 "$pid" 2>/dev/null || break
    curl -sf --max-time 1 -o /dev/null "$url" && { ready=1; break; }
    sleep 0.125
  done
  if [ "$ready" -ne 1 ]; then
    printf "ready:%s:trial%s|1\n" "$stack" "$trial"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    continue
  fi
  if [ "$conf" = true ]; then
    "$venv/bin/python" bench/conformance.py --stack "$stack" --port "$port" >>"$results/conformance.jsonl"
    printf "conformance:%s|%s\n" "$stack" "$?"
  fi
  for c in $levels; do
    "$venv/bin/python" bench/loadgen.py --url "$url" --stack "$stack" --trial "$trial" --concurrency "$c" \
      --duration "$duration" --warmup "$warmup" --timeout "$timeout" --expect-json "$expect" \
      --raw "$results/raw/${stack}__trial${trial}__c${c}.csv.gz" >>"$results/measurements.jsonl"
    printf "loadgen:%s:trial%s:c%s|%s\n" "$stack" "$trial" "$c" "$?"
  done
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
done <bench/.tmp/sessions.csv
# wait on a server we just signalled returns 143, which is not the walk failing.
# The gate rows above carry the real verdict.
exit 0
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['gate', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

-- Phase 6 -- crash durability. Both stacks acknowledge the same jobs and are
-- killed with SIGKILL; quackapi wrote them to its catalog-backed queue on the
-- request path, FastAPI held them in BackgroundTasks.
COPY (
  SELECT unnest(['quackapi', 'fastapi']) AS stack,
         unnest(['durable', 'volatile']) AS durability,
         unnest([quackapi_port, fastapi_port]) AS port,
         crash_jobs, crash_delay_ms, results_dir, duckdb_bin, venv, upstream_app
  FROM bench_run
) TO 'bench/.tmp/crash.csv' (FORMAT csv, HEADER false, DELIMITER '|', QUOTE '', ESCAPE '');

-- Two reads the crash walker cannot spell without quoting them through a shell:
-- the surviving job ids, and the acknowledgement the client reported. Both are
-- written here so the walker only ever runs a file.
COPY (SELECT 'COPY (SELECT len(array_agg(id)) AS survived FROM quackapi_jobs WHERE queue = ''bench_jobs'') TO ''/dev/stdout'' (FORMAT csv, HEADER false);' AS statement)
  TO 'bench/.tmp/survivors.sql' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '');

COPY (SELECT 'COPY (SELECT acknowledged FROM read_json(''bench/.tmp/crash_client.json'')) TO ''/dev/stdout'' (FORMAT csv, HEADER false);' AS statement)
  TO 'bench/.tmp/acknowledged.sql' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '');

CREATE OR REPLACE TABLE bench_crash AS
SELECT * FROM read_csv('bash -c ''
set -u
while IFS="|" read -r stack durability port jobs delay results duckdb venv app; do
  log=$results/${stack}_crash_server.log
  if [ "$stack" = quackapi ]; then
    db=$results/quackapi_crash.duckdb
    "$duckdb" -no-init -unsigned "$db" -c "INSTALL curl_httpfs FROM community; INSTALL httpfs_timeout_retry FROM community;" >"$log" 2>&1
    "$duckdb" -no-init -unsigned "$db" -f "$results/generated_routes.sql" -f bench/.tmp/serve_quackapi.sql >>"$log" 2>&1 &
  else
    UPSTREAM_SOURCE_PARENT=$(dirname "$app") FASTAPI_SURVIVORS=$results/fastapi_survivors.txt \
      "$venv/bin/uvicorn" fastapi_adapter:app --app-dir bench --host 127.0.0.1 --port "$port" --workers 1 --no-access-log >"$log" 2>&1 &
  fi
  pid=$!
  ready=0
  for _ in $(seq 1 160); do
    kill -0 "$pid" 2>/dev/null || break
    curl -sf --max-time 1 -o /dev/null "http://127.0.0.1:$port/users/rick?token=jessica" && { ready=1; break; }
    sleep 0.125
  done
  if [ "$ready" -ne 1 ]; then
    printf "ready:%s:crash|1\n" "$stack"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    continue
  fi
  "$venv/bin/python" bench/crash_client.py --port "$port" --jobs "$jobs" --delay-ms "$delay" >bench/.tmp/crash_client.json
  printf "crash_client:%s|%s\n" "$stack" "$?"
  cp bench/.tmp/crash_client.json "$results/${stack}_crash_client.json"
  acknowledged=$("$duckdb" -no-init -f bench/.tmp/acknowledged.sql)
  kill -KILL "$pid"; wait "$pid" 2>/dev/null
  if [ "$stack" = quackapi ]; then
    survived=$("$duckdb" -no-init -unsigned "$results/quackapi_crash.duckdb" -f bench/.tmp/survivors.sql)
  else
    sleep $(( delay / 1000 + 1 ))
    survived=$(wc -l <"$results/fastapi_survivors.txt" | tr -d " ")
  fi
  "$venv/bin/python" bench/crash_check.py --stack "$stack" --durability "$durability" \
    --attempted "$jobs" --acknowledged "$acknowledged" --survived "$survived" \
    --client-json "$(cat bench/.tmp/crash_client.json)" >>"$results/crash.jsonl"
  printf "crash:%s|%s\n" "$stack" "$?"
done <bench/.tmp/crash.csv
exit 0
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['gate', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

-- Phase 7 -- the report. report.sql reads its three raw files from the working
-- directory, which is how bench/test_report.sql drives it against a fixture.
CREATE OR REPLACE TABLE bench_report AS
SELECT * FROM read_csv('bash -c ''
set -u
report=$PWD/bench/report.sql
while IFS="|" read -r run_id results duckdb ext venv app rest; do
  case "$duckdb" in /*) ;; *) duckdb=$PWD/$duckdb ;; esac
  cp bench/.tmp/sessions.csv bench/.tmp/run.csv "$results/"
  ( cd "$results" && "$duckdb" -no-init -box -f "$report" >report.txt 2>&1 )
  printf "report|%s\n" "$?"
  cat "$results/report.txt" >&2
  printf "results=%s\n" "$results" >&2
done <bench/.tmp/run.csv
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['gate', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

-- The verdict. Every gate above is one row; a nonzero exit code in any of them
-- raises here, which is this duckdb process exiting nonzero.
WITH gates AS (
  FROM bench_prepare
  UNION ALL BY NAME FROM bench_sweep
  UNION ALL BY NAME FROM bench_crash
  UNION ALL BY NAME FROM bench_report
), failed AS (
  SELECT coalesce(array_agg(gate ORDER BY gate), []) AS gates FROM gates WHERE exit_code <> 0
)
SELECT CASE WHEN len(gates) = 0 THEN 'PASS: every bench gate held'
            ELSE error('FAIL: the run violated its own contract: ' || list_aggregate(gates, 'string_agg', ' ')) END AS verdict
FROM failed;
