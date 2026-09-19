# quackapi vs FastAPI: migration benchmark

This benchmark tests a bounded migration claim: quackapi reads an existing,
pinned FastAPI source tree with `quack_from_fastapi`, generated routes are
checked against that extraction, and those routes are measured against the
same FastAPI application.

It does **not** claim that `quack_from_fastapi` transpiles Python handlers. The
function currently returns route metadata only:

```text
method, path, handler_name, file, start_line, evidence
```

It does not return mounted router prefixes, dependencies, handler bodies, or
executable `CREATE ROUTE` SQL. `bench/migrate.py` therefore contains a small,
reviewable semantic map for the selected handlers. Startup fails unless every
mapped handler exists in the extractor output. The raw extraction and generated
SQL are preserved in every result directory.

## Pinned application

The source is FastAPI's official multi-file “Bigger Applications” application
at commit `50113da16fec53b66b80d75e80a89296de4fa5a5`. Eight files are fetched
from `fastapi/fastapi` and SHA-256 checked using `upstream_files.json`.

It was chosen because it is public, pinned, runnable without somebody else's
database, and exercises the exact real-tree problem a one-file fixture misses:
`APIRouter` prefixes, `include_router`, shared dependencies, header validation,
path parameters, and application-level error behavior. It is an official
example, not a production service; that limitation is intentional and reported.

On the pinned tree, the current extractor produces 8 route rows and 0 model
rows. Three extracted rows have the local path `/`; the real application mounts
them at `/`, `/items/`, and `/admin/`. That is why the manual map is explicit.

## What is measured

- Throughput and successful-response latency at 1, 8, 32, 64, and 320
  persistent connections. Every cell reports p50, p99, and max.
- Robustness: HTTP errors, contract failures, timeouts, connection resets, EOFs,
  and other no-response failures are counted separately.
- Error handling: a fixed nine-case conformance matrix checks success, missing
  query/header inputs, wrong tokens, not-found behavior, and forbidden updates.
- Crash durability: both services acknowledge 64 delayed jobs and are killed
  with `SIGKILL`. FastAPI uses `BackgroundTasks`; quackapi writes to its
  catalog-backed queue on the request path. Acknowledgements and surviving rows
  are counted after death.

The quackapi server is configured with `worker_threads := 32`, which is the only
HTTP dial: the pending queue follows it, so capacity is 32 active plus 256
queued. `QUACKAPI_MAX_PENDING_REQUESTS` pins the queue separately when a cell
wants the two budgets apart on purpose. The 64-connection cell is 2x the active
worker count. The 320-connection cell exceeds capacity deliberately; past it,
connections are shed with HTTP 503 naming the binding budget, never dropped
without a response.

The load generator is `bench/loadgen.py`, implemented with Python's standard
library. This avoids making k6 a hidden prerequisite. It preserves a gzip CSV
of every completed/error attempt and a JSON summary for every cell.

## Run

From the repository root, after building the extension:

```bash
GEN=ninja make release
bash bench/run.sh
```

`run.sh` creates its own virtual environment, downloads and verifies the pinned
application files, installs pinned FastAPI/uvicorn versions, starts both
services serially, and writes an immutable directory under `bench/results/`.
No pgEdge, Postgres, podman, psql, k6, or service from another repository is
used.

Useful controls:

```text
TRIALS=3
DURATION_SEC=3
WARMUP_SEC=0.5
CONCURRENCY_LEVELS="1 8 32 64 320"
REQUEST_TIMEOUT_SEC=12
CRASH_JOBS=64
CRASH_DELAY_MS=3000
```

For a quick mechanical smoke:

```bash
TRIALS=1 DURATION_SEC=0.5 WARMUP_SEC=0.1 \
  CONCURRENCY_LEVELS="1 8 32" bash bench/run.sh
```

Each run preserves:

```text
extraction.json
generated_routes.sql
measurements.jsonl
raw/*.csv.gz
conformance.jsonl
crash.jsonl
environment.txt
*_server.log
```

## Scope limits

- The benchmark uses no Postgres path. If a future scenario adds Postgres, the
  quackapi side must use `postgres_query()`; `ATTACH (TYPE postgres)` is not an
  acceptable comparand.
- The migrated handlers are constant/in-memory handlers. Python business logic,
  dependency graphs, and stores are not automatically translated.
- Localhost results describe this machine and this build only. They are not a
  production capacity forecast.
- `BackgroundTasks` and the quackapi queue make different promises. Their
  request latency is shown, but the crash result is the relevant comparison.

The older Markdown files in this directory are retained as historical notes;
they are not prerequisites or current benchmark claims.
