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
quackapi_dequeue(queue VARCHAR [, n INTEGER]) → TABLE(id, queue, payload, status, attempts, max_attempts, visible_at, last_error, delivery_generation)
quackapi_ack(queue VARCHAR, job_id BIGINT, delivery_generation BIGINT) → BOOLEAN
quackapi_nack(queue VARCHAR, job_id BIGINT, delivery_generation BIGINT [, requeue BOOLEAN [, error VARCHAR]]) → VARCHAR  -- pending, dead, or stale
quackapi_renew(queue VARCHAR, job_id BIGINT, delivery_generation BIGINT) → BOOLEAN
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
| dequeue | Atomic `UPDATE … WHERE id=(SELECT … LIMIT 1) RETURNING`: `status='running'`, `attempts++`, `delivery_generation++`, `visible_at=now()+visibility_timeout` |
| redelivery | Each expired delivery is recovered independently. A timed-out final attempt becomes `dead`; earlier attempts become claimable again. |
| ack | `status='done'` only for the supplied generation while its lease is active; `false` means stale, expired, or already completed. |
| nack (requeue) | `pending` + exponential backoff, or `dead` if `attempts >= max_attempts`; returns `stale` when the supplied generation no longer owns the delivery. |
| nack (requeue=false) | `dead` immediately |
| renew | Extends the configured visibility timeout only for the supplied generation while its current lease is active. Returns `false` after expiry or supersession. |

The generation fences every delivery transition. A worker that times out and is
superseded cannot acknowledge, retry, or renew the newer delivery. Queue
transitions serialize on a per-database lock shared by static and loadable
extension copies; DuckDB remains single-writer per database file. For multi-node
workers use a real broker (Redis / NATS).

## Delivery-generation migration

This release adds the persisted `delivery_generation` column to existing
`quackapi_jobs` tables automatically. The old two-argument `quackapi_ack` and
old `quackapi_nack` forms have been removed because they could let a stale worker
change a redelivered job. Update workers to carry the generation returned by
`quackapi_dequeue` through every delivery transition.

## Worker = `cronjob`

Do **not** expect a C++ thread pool in quackapi. `quackapi_queue_worker` hands the
drain to the community `cronjob` extension, which is the scheduler:

```sql
INSTALL cronjob FROM community;   -- setup, once. quackapi never downloads it.
LOAD cronjob;

CREATE QUEUE emails;
SELECT * FROM quackapi_queue_worker('emails');
-- queue   job_id  schedule        sql
-- emails  task_0  */1 * * * * *   SELECT quackapi_ack('emails', id, delivery_generation) …
```

`cron_jobs()` lists them and `cron_delete(job_id)` removes one — quackapi adds no
wrapper for either. `sql := '…'` replaces the default ack-drain with your own
handler SQL; `schedule := '…'` and `batch := n` are the other two knobs.

An external scheduler exists because ordinary processes die. A server that is
already obliged to stay online gives `cron` that uptime for free, which is why
the runner lives in the same process as the queue.

Without `cronjob` loaded, `quackapi_queue_worker` refuses and names it. Or drain
from an HTTP route (`POST /drain`) that runs the same SELECT.

## Redis

The community Redis extension remains the "I already run Redis" option
(LPUSH-style). Native table-queue is the zero-dependency default.
