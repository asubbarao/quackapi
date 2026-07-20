# quackapi

HTTP JSON APIs from SQL, inside DuckDB. Routes are DDL, handlers are SELECTs,
and validation comes from the database's own type system.

```sql
LOAD quackapi;

CREATE ROUTE hello GET '/hello' AS SELECT 'world' AS msg;
CREATE ROUTE item  GET '/items/:id' AS SELECT $id::INT AS id;

SELECT * FROM quackapi_serve(8000);
```

```
$ curl http://127.0.0.1:8000/hello
[{"msg":"world"}]

$ curl http://127.0.0.1:8000/items/42
[{"id":42}]

$ curl http://127.0.0.1:8000/items/abc
{"detail":[{"loc":["path","id"],"msg":"Input should be a valid INTEGER","type":"type_error"}]}
```

No app server, no ORM, no schema duplication: the query already types the
response, so the framework doesn't need a model layer to re-declare it.

## API

| Surface | What it does |
|---|---|
| `CREATE [OR REPLACE] ROUTE <name> <METHOD> '<pattern>' [STATUS <n>] AS <select>` | Register an endpoint. `:param` / `{param}` path segments and query-string params bind to the handler's named parameters (`$param`). |
| `DROP ROUTE <name>` | Remove an endpoint. Changes apply live while serving. |
| `quackapi_serve([port], host := '127.0.0.1')` | Serve on background threads; the shell stays usable. |
| `quackapi_stop([port])` | Stop one server, or all. |
| `quackapi_routes()` | Inspect the route registry. |
| `quackapi_servers()` | List running servers. |
| `CREATE [OR REPLACE] QUEUE <name> [WITH (max_attempts=3, visibility_timeout='30s', backoff_base_seconds=2)]` | Register a durable, broker-less job queue. Jobs live in the `quackapi_jobs` table (your `.db` file). |
| `DROP QUEUE <name>` | Unregister a queue (job rows are kept). |
| `quackapi_enqueue(queue, payload [, max_attempts])` | Enqueue a job (payload is JSON text); returns `job_id`. Callable from a route handler. |
| `quackapi_dequeue(queue [, n])` | Atomically claim up to `n` ready jobs (visibility lease). |
| `quackapi_ack(queue, job_id)` / `quackapi_nack(queue, job_id [, requeue [, error]])` | Complete or retry/dead-letter a claimed job. |
| `quackapi_queues()` | Inspect name, depth, in_flight, dead (+ options). |

### Durable job queue (broker-less)

No Redis, no NATS: your job queue is a table. Semantics match the SQS/BullMQ-lite
tier — durable enqueue, atomic claim with a visibility timeout, ack, nack with
exponential backoff, and a dead-letter status after `max_attempts`.

```sql
LOAD quackapi;

CREATE QUEUE emails WITH (max_attempts=5, visibility_timeout='30s');

-- From a route (or any SQL):
CREATE ROUTE enqueue POST '/jobs' STATUS 201 PARAM payload VARCHAR
  AS SELECT quackapi_enqueue('emails', $payload) AS job_id;

-- Worker tick (run from a session, or schedule via the community cronjob ext):
SELECT id, payload, quackapi_ack('emails', id) AS acked
FROM quackapi_dequeue('emails', 10);
```

**Worker story — compose `cronjob`, do not build a C++ thread pool:**

```sql
INSTALL cronjob FROM community;
LOAD cronjob;

-- Drain one job per second: claim → process → ack (or nack on failure).
SELECT cron($$
  SELECT quackapi_ack('emails', id)
  FROM quackapi_dequeue('emails', 1)
  -- add your processing here; on failure use quackapi_nack('emails', id, true, 'err')
$$, '*/1 * * * * *');
```

Redis remains available when you already run a broker; the native table-queue is
the zero-dependency default for single-node BackgroundTasks / Celery-lite work.

## Behavior

- **Validation**: request params are cast to the types the prepared handler
  expects; failures return `422` with a FastAPI-shaped `detail` body. Missing
  required params → `422`; unknown path → `404`; wrong method → `405`.
- **Responses**: JSON array of row objects. JSON types follow column types —
  numbers stay numbers, booleans stay booleans, lists/structs nest, `NULL` is
  `null`.
- **State**: the route registry lives with the database instance — nothing is
  written to your catalog. Serving uses DuckDB's bundled httplib (the same
  transport as the core `quack` RPC extension) with a listener thread and
  worker pool.

## Build

```sh
git clone --recurse-submodules https://github.com/asubbarao/quackapi
cd quackapi
GEN=ninja make release
build/release/duckdb   # extension is pre-loaded in this shell
```

Run tests: `make test`

## License

MIT
