# CREATE QUEUE — background jobs without Redis

Jobs live in an ordinary DuckDB table: **`quackapi_jobs`**. No external broker. Enqueue from a route, drain with a worker route (or scheduled SQL).

All examples run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

---

## Create a queue

```sql
CREATE QUEUE default WITH (
  max_attempts=3,
  visibility_timeout='30s',
  backoff_base_seconds=2
);
```

| Option | Default | Meaning |
|--------|---------|---------|
| `max_attempts` | 3 | After this many claims, nack → `dead` |
| `visibility_timeout` | 30s | Lease length while `running` (`'30s'`, `'5m'`, or bare seconds) |
| `backoff_base_seconds` | 2 | Nack delay ≈ base^attempts (`0` = immediate) |

```sql
SELECT name, depth, in_flight, dead,
       max_attempts, visibility_timeout_sec, backoff_base_sec
FROM quackapi_queues();
```

---

## HTTP: enqueue + drain worker

```sql
CREATE QUEUE default WITH (max_attempts=3, visibility_timeout='30s');

CREATE ROUTE enqueue POST '/jobs' STATUS 201
  PARAM payload VARCHAR
  AS
SELECT quackapi_enqueue('default', $payload::VARCHAR) AS job_id;

-- Worker tick: claim up to 10 jobs and ack each in one SELECT
CREATE ROUTE drain POST '/drain' AS
SELECT id AS job_id,
       'processed:' || payload AS result,
       quackapi_ack('default', id) AS acked
FROM quackapi_dequeue('default', 10);

CREATE ROUTE results GET '/results' AS
SELECT id AS job_id, payload, status
FROM quackapi_jobs
WHERE status = 'done'
ORDER BY id;

CREATE ROUTE stats GET '/stats' AS
SELECT name, depth, in_flight, dead FROM quackapi_queues();
```

Live run:

```sh
curl -X POST http://127.0.0.1:8000/jobs \
  -H 'Content-Type: application/json' \
  -d '{"payload":"{\"task\":\"email\"}"}'
# [{"job_id":1}]
# HTTP 201

curl http://127.0.0.1:8000/stats
# [{"name":"default","depth":1,"in_flight":0,"dead":0}]

# httplib wants a body on POST — send empty JSON
curl -X POST http://127.0.0.1:8000/drain \
  -H 'Content-Type: application/json' -d '{}'
# [{"job_id":1,"result":"processed:{\"task\":\"email\"}","acked":true}]

curl http://127.0.0.1:8000/results
# [{"job_id":1,"payload":"{\"task\":\"email\"}","status":"done"}]
```

---

## SQL API (no HTTP)

```sql
SELECT quackapi_enqueue('default', '{"task":"send_email"}');
-- → job_id (BIGINT)

SELECT id, payload, status, attempts
FROM quackapi_dequeue('default', 10);
-- claims: status=running, attempts++, visibility lease

SELECT quackapi_ack('default', 1);
-- true if job was running

SELECT quackapi_nack('default', 1, true, 'try_again');
-- 'pending' (retry) or 'dead' at max_attempts

SELECT quackapi_nack('default', 1, false, 'no_retry');
-- straight to 'dead'
```

Payload is stored as **VARCHAR** (JSON text). You may also pass `JSON` when the json extension is loaded.

Per-job max attempts override:

```sql
SELECT quackapi_enqueue('default', '{"z":1}', 1);
```

---

## Semantics

| Op | Behavior |
|----|----------|
| enqueue | `pending`, visible now |
| dequeue | Exclusive claim → `running` + lease |
| redelivery | `running` rows with expired lease are claimable again |
| ack | `done` |
| nack (requeue) | `pending` + backoff, or `dead` at max_attempts |
| nack (no requeue) | `dead` immediately |

**Lifecycle note:** queue *options* live on the database instance (re-run `CREATE QUEUE` after reopen). Job *rows* in `quackapi_jobs` are durable in your `.db` file.

Concurrency: DuckDB is single-writer per file. Claims serialize on the write lock — correct for one process. For multi-node workers, use an external broker.

---

## Worker options

1. **HTTP drain route** (above) — cron or a second process POSTs `/drain`.  
2. **Scheduled SQL** — community `cronjob` extension (or any external scheduler) running the same `SELECT … FROM quackapi_dequeue … ack`.  
3. **Interactive shell** — run dequeue/ack by hand while developing.

There is no built-in C++ thread pool. The queue is storage + claim primitives; **you** schedule the worker.

---

## Drop

```sql
DROP QUEUE default;
-- registry entry removed; quackapi_jobs rows for that queue remain on disk
```

---

## Next

- [CREATE STREAM](stream.md) — push events while jobs run  
- Deeper notes: [QUEUE.md](../QUEUE.md)
