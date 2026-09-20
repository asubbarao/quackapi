# FastAPI versus QuackAPI: production matrix

The repository now has a paired HTTP corpus under
[`test/ultra`](../test/ultra). It runs the
same cases against a FastAPI reference app and a QuackAPI SQL fixture, then
records per-stack latency and semantic failures. The fixed cases cover the
contracts that are easy to compare directly; the existing SQL and benchmark
suites cover the database, queue, policy, and throughput dimensions that need
their own oracles.

FastAPI's own feature list identifies OpenAPI/JSON Schema, Pydantic validation,
security schemes, dependency injection, background tasks, WebSockets,
startup/shutdown events, CORS, gzip, static files, and streaming responses as
important framework surfaces. Those are the source categories for this matrix:
[FastAPI features](https://fastapi.tiangolo.com/features/),
[testing](https://fastapi.tiangolo.com/tutorial/testing/), and
[response models](https://fastapi.tiangolo.com/tutorial/response-model/).

The production matrix also tests deployment behavior instead of treating a
single-process benchmark as a production claim. FastAPI's deployment guidance
calls out HTTPS termination, startup supervision, restarts, worker replication,
per-process memory, and resource utilization:
[deployment concepts](https://fastapi.tiangolo.com/deployment/concepts/) and
[server workers](https://fastapi.tiangolo.com/deployment/server-workers/).
Proxy forwarding is part of the security surface because forwarded headers are
not trusted by default:
[behind a proxy](https://fastapi.tiangolo.com/advanced/behind-a-proxy/).

## Acceptance rules

- A transport failure is a failure even when the expected status is absent.
- A validation response must contain a structured `detail` list with `loc`,
  `msg`, and `type`; a generic 422 is not equivalent to FastAPI/Pydantic.
- A response-model test must prove that undeclared fields are absent. This is
  a security property, not a cosmetic serialization check.
- QuackAPI's default row-array envelope is normalized only where the case says
  `json_object`; the test does not hide arbitrary shape differences.
- Optional extension probes are version-aware. An extension compiled for a
  different DuckDB version is `SKIP` with evidence, never a green result.
- Throughput claims require the existing equal aggregate worker/connection
  budget, valid `loadgen.py` cells, retained latency samples, and
  committed-write checks.

## Current extension composition targets

The local DuckDB extension inventory includes these useful production pieces:

| Extension | Use in the matrix | Contract to preserve |
|---|---|---|
| `httpfs_timeout_retry` | per-operation file timeout and retry settings | bounded retry count and operation-specific timeout |
| `cache_prewarm` | warm remote/file data before a hot route | explicit warmup evidence and no hidden unbounded work |
| `cache_httpfs` | bounded metadata/data/file-handle caches | cache size, eviction, validation, and stale-read policy |
| `http_stats` | HTTP filesystem observability | retain timing/error evidence with the request/run |
| `finetype` | data-driven type inference for migration/bridge tooling | inference is evidence for schema generation, not authorization |
| `query_condition_cache` | repeated predicate compilation acceleration | invalidation and correctness under changing data |
| `table_guard` | table access guardrails | fail closed and test both allowed and denied relations |

QuackAPI should compose these extensions when they are present and keep its
core community-extension load path independent of them. The probe script makes
that boundary executable.

## Running it

The default run starts both reference servers, executes the fixed corpus plus
deterministic fuzz cases, records raw per-request rows and latency quantiles,
then probes the locally installed optional extensions:

```sh
build/release/duckdb -no-init -f test/ultra/run.sql
```

Use `FULL=1 build/release/duckdb -no-init -f test/ultra/run.sql` to add the repository's 85-case HTTP
conformance corpus and scorecard. The paired corpus is a contract gate: a
nonzero exit means a request, response, validation, security, or transport
expectation failed. Generated results are intentionally ignored; copy a
reviewed summary into a release report when publishing measurements.

The current fixture covers 64 cases (128 requests), including deterministic
fuzzing of path integers, malformed JSON, nulls, extra fields, query bounds,
headers, cookies, auth, redirects, response filtering, gzip, NDJSON, SSE,
CSV, CORS, and OpenAPI. The latest clean run reported 128/128 request checks
passing. One expected semantic difference is recorded rather than hidden:
FastAPI accepts arbitrarily large Python integers while QuackAPI's DuckDB
integer binding rejects values outside the declared 64-bit range. The case
expects QuackAPI's 422 and FastAPI's route-level 404 for that overflow input.

The extension probe is version-aware. It loads matching local builds of
`httpfs_timeout_retry`, `cache_prewarm`, `cache_httpfs`, `http_stats`,
`finetype`, `query_condition_cache`, and `table_guard`, verifies the settings
and function surfaces, and records a PASS/SKIP manifest. A skipped extension
is evidence of a DuckDB ABI mismatch, not a false green result.
