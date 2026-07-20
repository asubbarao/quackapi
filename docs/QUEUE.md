# quackapi durable job queue

Broker-less, single-file job queue built into quackapi. Your queue is a table —
no Redis, no NATS required.

## Surface

```sql
CREATE [OR REPLACE] QUEUE <name>
  [WITH (
     max_attempts=<n>,              -- default 3; after this, job → dead
     visibility_timeout='30s'|30,   -- claim lease (s/m/h or seconds)
     backoff_base_seconds=<n>       -- default 2; nack delay = base^attempts (0 = immediate)
  )];

DROP QUEUE <name>;

quackapi_enqueue(queue VARCHAR, payload VARCHAR|JSON [, max_attempts INTEGER]) → BIGINT
quackapi_dequeue(queue VARCHAR [, n INTEGER]) → TABLE(id, queue, payload, status, attempts, max_attempts, visible_at, last_error)
quackapi_ack(queue VARCHAR, job_id BIGINT) → BOOLEAN
quackapi_nack(queue VARCHAR, job_id BIGINT [, requeue BOOLEAN [, error VARCHAR]]) → VARCHAR  -- new status
quackapi_queues() → TABLE(name, depth, in_flight, dead, max_attempts, visibility_timeout_sec, backoff_base_sec)
```

## Storage

- **Jobs** live in `quackapi_jobs` (normal DuckDB table + `quackapi_job_seq`).
  Survives process restart in the user's `.db` file.
- **Queue options** live on the `DatabaseInstance` (same lifecycle as routes —
  re-run `CREATE QUEUE` after reopen).
- Payload is stored as JSON **text** (`VARCHAR`) so `LOAD json` is not required
  at runtime. Cast to `JSON` when the json extension is loaded.

## Semantics

| Op | Behavior |
|----|----------|
| enqueue | `status='pending'`, `visible_at=now()` |
| dequeue | Atomic `UPDATE … WHERE id=(SELECT … LIMIT 1) RETURNING`: `status='running'`, `attempts++`, `visible_at=now()+visibility_timeout` |
| redelivery | Rows with `status='running' AND visible_at <= now()` are claimable again |
| ack | `status='done'` (only if currently `running`) |
| nack (requeue) | `pending` + exponential backoff, or `dead` if `attempts >= max_attempts` |
| nack (requeue=false) | `dead` immediately |

Concurrency: DuckDB is single-writer per database file. Claims serialize on the
write lock — correct for one process / one file (BackgroundTasks tier). For
multi-node workers use a real broker (Redis / NATS).

## Worker = compose `cronjob`

Do **not** expect a C++ thread pool in quackapi. Schedule SQL:

```sql
INSTALL cronjob FROM community;
LOAD cronjob;

SELECT cron($$
  SELECT quackapi_ack('emails', id)
  FROM quackapi_dequeue('emails', 1)
$$, '*/1 * * * * *');
```

Or drain from an HTTP route (`POST /drain`) that runs the same SELECT.

## Redis

The community Redis extension remains the "I already run Redis" option
(LPUSH-style). Native table-queue is the zero-dependency default.
