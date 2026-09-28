# Ultra parity and production matrix

This suite is the release gate for comparing a SQL-native QuackAPI service with
a representative FastAPI application. It has three layers:

1. `paired_driver.py` sends the same contract cases to both live servers and
   checks status, validation locations, response filtering, headers, content
   negotiation, CORS, compression, streaming, redirects, and OpenAPI.
2. `run_extensions.sh` discovers version-matched DuckDB extensions and probes
   `httpfs_timeout_retry`, `cache_prewarm`, `cache_httpfs`, `http_stats`,
   `finetype`, `query_condition_cache`, and `table_guard`. A version mismatch
   is recorded as `SKIP`, never as a false pass.
3. The existing SQL suite, HTTP conformance suite, and benchmark matrix remain
   the deeper regression and throughput gates. `run.sh` orchestrates all of
   them when `FULL=1`.

## Run the paired matrix

```bash
bash test/ultra/run_pair.sh
```

The result directory contains `results.jsonl`, `summary.json`, and both server
logs. Set `NO_FUZZ=1` for the fixed corpus only. The default adds deterministic
path, integer, malformed-body, null, extra-field, and list-boundary cases.

## Run the extension probes

```bash
bash test/ultra/run_extensions.sh
```

The script selects a DuckDB binary whose version matches the locally installed
optional extensions. This matters because DuckDB rejects loading an extension
compiled for another engine version. The probe records the settings and
function signatures so a future run can compare the actual extension surface.

## What this matrix measures

| Dimension | Paired cases | Production follow-up |
|---|---|---|
| Routing | path captures, 404/405/Allow, HEAD | fixed and concurrent k6 cells |
| Validation | typed path/query/body values, null/missing/extra fields, bounds, malformed JSON | SQLLogicTest aggregate-error and overflow corpus |
| Security | bearer/API-key behavior, response-field filtering | row policies, masking, JWT claims, rate limits, queue fencing |
| Responses | JSON objects/lists, HTML, text, CSV, NDJSON, SSE, redirects | content caps, compression, disconnect cancellation |
| Transport | CORS preflight, gzip negotiation, OpenAPI | keep-alive, admission queue, timeout and retry faults |
| Operations | deterministic logs and machine-readable scorecard | equal worker/connection budgets and write accounting |

WebSockets are intentionally a separate capability result. FastAPI documents
WebSockets as a first-class Starlette transport, while QuackAPI's bundled
transport exposes SSE and rejects WebSocket registration. That is a transport
boundary to measure honestly, not a missing JSON-route test.

