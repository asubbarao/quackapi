-- Paired FastAPI/QuackAPI matrix: both stacks serve the same routes, the driver
-- puts the same requests to each and compares the answers.
--
-- Run from the repository root:
--   build/release/duckdb -no-init -f test/ultra/run_pair.sql
--
-- Controls: QUACK_PORT (18772), FASTAPI_PORT (18773), RESULTS_DIR,
-- FASTAPI_VENV (bench/.venv), DUCKDB (build/release/duckdb),
-- QUACKAPI_EXT (build/release/extension/quackapi/quackapi.duckdb_extension),
-- NO_FUZZ (set to skip the fuzz cases).
--
-- quackapi runs behind a FIFO for the same reason the conformance harness does:
-- quackapi_serve blocks, and `duckdb -c` would parse the whole script first.
-- Every knob is resolved once here and handed to the shell as a file, so the
-- shell resolves no defaults of its own and never has to quote SQL.

INSTALL shellfs FROM community;
LOAD shellfs;

CREATE OR REPLACE TABLE staging AS
SELECT * FROM read_csv('bash -c ''mkdir -p test/ultra/.tmp; printf "mkdir|%s\n" "$?"'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['gate', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

CREATE OR REPLACE TABLE pair_config AS
SELECT coalesce(try_cast(getenv('QUACK_PORT') AS INTEGER), 18772)   AS quack_port,
       coalesce(try_cast(getenv('FASTAPI_PORT') AS INTEGER), 18773) AS fastapi_port,
       coalesce(nullif(getenv('FASTAPI_VENV'), ''), 'bench/.venv')  AS venv,
       coalesce(nullif(getenv('DUCKDB'), ''), 'build/release/duckdb') AS duckdb_bin,
       coalesce(nullif(getenv('QUACKAPI_EXT'), ''),
                'build/release/extension/quackapi/quackapi.duckdb_extension') AS extension_path,
       coalesce(nullif(getenv('RESULTS_DIR'), ''), '') AS results_dir,
       coalesce(nullif(getenv('NO_FUZZ'), ''), '') AS no_fuzz;

COPY pair_config TO 'test/ultra/.tmp/pair_config.csv' (FORMAT csv, HEADER false, DELIMITER '|', QUOTE '', ESCAPE '');

-- The whole FIFO session as one file: the installs, the LOAD of the built
-- extension, the routes read verbatim, then the serve statement.
-- quackapi_serve(port, host, health_routes, access_log, enable_logging,
--   compression, cors_origins, worker_threads, max_pending_requests, pg_dsn,
--   block): only the parameters the matrix depends on are set, the rest keep
--   their quackapi defaults.
COPY (
  SELECT 'INSTALL curl_httpfs FROM community;' || chr(10)
      || 'INSTALL httpfs_timeout_retry FROM community;' || chr(10)
      || 'LOAD ''' || replace(extension_path, '''', '''''') || ''';' || chr(10)
      || (SELECT content FROM read_text('test/ultra/routes.sql')) || chr(10)
      || 'SELECT * FROM quackapi_serve(' || quack_port
      || ', host := ''127.0.0.1'', health_routes := false, access_log := false'
      || ', enable_logging := false, compression := true'
      || ', cors_origins := ''http://example.test'');' AS session
  FROM pair_config
) TO 'test/ultra/.tmp/quack_session.sql' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '');

CREATE OR REPLACE TABLE pair_run AS
SELECT * FROM read_csv('bash -c ''
set -u
while IFS="|" read -r quack_port fastapi_port venv duck ext results no_fuzz; do
  [ -n "$results" ] || results=test/ultra/results/$(date -u +%Y%m%dT%H%M%SZ)-$$
  FIFO=${TMPDIR:-/tmp}/quackapi_ultra_${quack_port}_$$.fifo
  if [ ! -x "$duck" ]; then echo "missing DuckDB binary: $duck" >&2; printf "duckdb|2\n"; continue; fi
  if [ ! -f "$ext" ]; then echo "missing quackapi extension: $ext" >&2; printf "extension|2\n"; continue; fi
  if [ ! -x "$venv/bin/uvicorn" ]; then echo "missing FastAPI venv: $venv" >&2; printf "venv|2\n"; continue; fi
  case "$venv" in /*) uvicorn=$venv/bin/uvicorn ;; *) uvicorn=$PWD/$venv/bin/uvicorn ;; esac

  mkdir -p "$results"
  mkfifo "$FIFO"
  "$duck" -no-init -unsigned <"$FIFO" >"$results/quackapi.log" 2>&1 &
  QP=$!
  exec 3>"$FIFO"
  cat test/ultra/.tmp/quack_session.sql >&3

  ( cd test/ultra && PYTHONPATH=$PWD "$uvicorn" fastapi_app:app --host 127.0.0.1 --port "$fastapi_port" --workers 1 ) >"$results/fastapi.log" 2>&1 &
  FP=$!

  ready=0
  for _ in $(seq 1 120); do
    q=0; f=0
    curl -sf --max-time 1 -o /dev/null "http://127.0.0.1:$quack_port/matrix/health" && q=1
    curl -sf --max-time 1 -o /dev/null "http://127.0.0.1:$fastapi_port/matrix/health" && f=1
    if [ "$q" -eq 1 ] && [ "$f" -eq 1 ]; then ready=1; break; fi
    kill -0 "$QP" 2>/dev/null || break
    kill -0 "$FP" 2>/dev/null || break
    sleep 0.1
  done
  if [ "$ready" -ne 1 ]; then
    cat "$results/quackapi.log" "$results/fastapi.log" >&2
    exec 3>&-
    kill "$FP" "$QP" 2>/dev/null; wait "$FP" 2>/dev/null; wait "$QP" 2>/dev/null
    rm -f "$FIFO"
    printf "ready|3\n"; continue
  fi

  if [ -n "$no_fuzz" ]; then
    python3 test/ultra/paired_driver.py --quack "http://127.0.0.1:$quack_port" --fastapi "http://127.0.0.1:$fastapi_port" --out "$results" --no-fuzz
  else
    python3 test/ultra/paired_driver.py --quack "http://127.0.0.1:$quack_port" --fastapi "http://127.0.0.1:$fastapi_port" --out "$results"
  fi
  printf "paired_driver|%s\n" "$?"

  printf "SELECT * FROM quackapi_stop();\n" >&3
  exec 3>&-
  sleep 0.3
  kill "$FP" "$QP" 2>/dev/null; wait "$FP" 2>/dev/null; wait "$QP" 2>/dev/null
  rm -f "$FIFO"
  printf "paired matrix results: %s\n" "$results" >&2
done <test/ultra/.tmp/pair_config.csv
'' |',
  header := false, delim := '|', quote := '', escape := '',
  names := ['gate', 'exit_code'], types := ['VARCHAR', 'INTEGER']);

WITH recorded AS (
  SELECT coalesce(array_agg(gate ORDER BY gate) FILTER (WHERE exit_code <> 0), []) AS failed,
         coalesce(array_agg(gate), []) AS gates
  FROM (FROM staging UNION ALL BY NAME FROM pair_run)
)
SELECT CASE
  WHEN len(gates) = 0 THEN error('FAIL: the paired matrix reported nothing')
  WHEN len(failed) > 0 THEN error('FAIL: ' || list_aggregate(failed, 'string_agg', ' '))
  ELSE 'PASS: the paired FastAPI/QuackAPI matrix agrees' END AS verdict
FROM recorded;
