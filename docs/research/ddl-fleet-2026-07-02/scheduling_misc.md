# quackapi DDL Research: Scheduling & Catch-All CREATE-* Sweep

**Date:** 2026-07-02  
**Purpose:** Inform quackapi's CREATE JOB / CREATE CRON design and identify any other CREATE-* DDL worth stealing.

---

## PART 1 — SCHEDULING DDL

### 1a. Does a reusable DuckDB cron extension exist?

**YES. `cronjob` is in the official DuckDB community extensions registry.**

```sql
INSTALL cronjob FROM community;
LOAD cronjob;
```

Source: https://duckdb.org/community_extensions/extensions/cronjob  
GitHub: https://github.com/quackscience/duckdb-extension-cronjob

#### API

| Function | Signature | Returns |
|----------|-----------|---------|
| `cron()` | `cron(query VARCHAR, schedule VARCHAR) → VARCHAR` | job_id (e.g. `'task_0'`) |
| `cron_delete()` | `cron_delete(job_id VARCHAR) → void` | — |
| `cron_jobs()` | table function | `job_id, query, schedule, next_run, status, last_run, last_result` |

#### Cron expression format (6 fields)
```
second  minute  hour  day_of_month  month  day_of_week
(0-59)  (0-59)  (0-23)  (1-31)    (1-12)   (0-6, or MON-SUN)
```

Supported operators: `*` (any), `?` (no specific value), `-` (range), `/` (step), `,` (list), named days.

#### Examples
```sql
-- Schedule a cleanup every 15 seconds during hours 1-4
SELECT cron('DELETE FROM logs WHERE ts < now() - INTERVAL 7 DAY', '*/15 * 1-4 * * *');

-- Query every 2 hours at :00:00
SELECT cron('INSERT INTO snapshots SELECT * FROM live_data', '0 0 */2 * * *');

-- Inspect all jobs
SELECT * FROM cron_jobs();

-- Cancel a job
SELECT cron_delete('task_0');
```

#### Honest runtime assessment
- Executes queries "while your DuckDB process is active" — implies it runs a background thread within the in-process DuckDB instance.
- **No documented persistence across session restarts** — jobs are lost when the DuckDB process exits.
- Threading model not documented (no source-level docs found).
- Marked **experimental / USE AT YOUR OWN RISK** — not production-ready.

#### Second cron extension: `duckdb-cron-expression`
There is a separate read-only extension (`rustyconover/duckdb-cron-extension`) that only *calculates* upcoming cron-expression timestamps — it does NOT execute anything. Not useful for scheduling.

---

### 1b. pg_cron (PostgreSQL)

**Concept:** Extension that runs inside the Postgres background worker process.  
Source: https://github.com/citusdata/pg_cron

```sql
-- Syntax
SELECT cron.schedule('<job_name>', '<cron_expr>', '<sql_command>');
SELECT cron.schedule('<cron_expr>', '<sql_command>');  -- returns numeric job_id

-- Examples
SELECT cron.schedule('vacuum-job', '30 3 * * 6', 'VACUUM ANALYZE orders');
SELECT cron.schedule('0 10 * * *', 'CALL generate_daily_report()');
SELECT cron.schedule('*/5 * * * *', $$DELETE FROM events WHERE age > INTERVAL '7 days'$$);

-- Unschedule
SELECT cron.unschedule('vacuum-job');
SELECT cron.unschedule(42);  -- by job_id

-- Inspect
SELECT * FROM cron.job;
SELECT * FROM cron.job_run_details;
```

**Cron fields (5):** `min hour dom month dow`  
Also supports `'[1-59] seconds'` for sub-minute intervals.  
Special: `$` = last day of month.  
**All schedules in UTC.**

**Key design choices:**
- Jobs are stored in `cron.job` table — **persistent across restarts**.
- Runs in a dedicated background worker, separate from connection pool.
- `job_run_details` gives full history with start/end time and return status.
- Can run jobs in any database on the server (not just the one it's loaded in).

---

### 1c. Snowflake CREATE TASK

Source: https://docs.snowflake.com/en/sql-reference/sql/create-task

```sql
CREATE [ OR REPLACE ] TASK [ IF NOT EXISTS ] <name>
    [ WAREHOUSE = <wh> | USER_TASK_MANAGED_INITIAL_WAREHOUSE_SIZE = '<xs|s|m|l|xl|xxl>' ]
    [ SCHEDULE = '<num> { HOURS | MINUTES | SECONDS }' 
               | 'USING CRON <expr> <time_zone>' ]
    [ OVERLAP_POLICY = { NO_OVERLAP | ALLOW_CHILD_OVERLAP | ALLOW_ALL_OVERLAP } ]
    [ USER_TASK_TIMEOUT_MS = <num> ]
    [ SUSPEND_TASK_AFTER_NUM_FAILURES = <num> ]
    [ ERROR_INTEGRATION = <integration_name> ]
    [ SUCCESS_INTEGRATION = <integration_name> ]
    [ AFTER <predecessor_task> [, ...] ]
    [ WHEN <boolean_expr> ]
  AS
    <sql_or_stored_proc>;

-- Must explicitly RESUME after CREATE (tasks start suspended)
ALTER TASK my_task RESUME;
EXECUTE TASK my_task;  -- manual trigger
```

**Key design concepts:**

1. **AFTER (dependency DAG):** Child tasks run after all predecessors succeed. Root task has SCHEDULE, children have AFTER only. Max 100 predecessors and 100 children per task.

2. **WHEN condition:** Run task only if condition is true at fire time.
   ```sql
   WHEN SYSTEM$STREAM_HAS_DATA('my_stream')
   WHEN NOT SYSTEM$GET_PREDECESSOR_RETURN_VALUE('upstream_task')::BOOLEAN
   ```

3. **Two compute modes:**
   - `WAREHOUSE = wh` — classic user-managed warehouse
   - `USER_TASK_MANAGED_INITIAL_WAREHOUSE_SIZE` — serverless (Snowflake auto-scales)

4. **Failure handling:** `SUSPEND_TASK_AFTER_NUM_FAILURES = 5` auto-suspends on repeated failures. `ERROR_INTEGRATION` fires notifications.

5. **OVERLAP_POLICY:** Controls whether a new run starts if the previous is still running.

**Key quackapi steal:**
- AFTER-based dependency graph (DAG)
- WHEN condition for conditional execution
- Separation of "schedule owner" (root) vs "dependency-only tasks" (children)
- Auto-suspend on N consecutive failures
- Error/success notification integrations

---

### 1d. Snowflake CREATE DYNAMIC TABLE

Source: https://docs.snowflake.com/en/sql-reference/sql/create-dynamic-table

```sql
CREATE [ OR REPLACE ] DYNAMIC TABLE <name>
    TARGET_LAG = { '<duration>' | DOWNSTREAM }
    WAREHOUSE = <wh>
  AS
    SELECT ...;
```

**Core concept:** TARGET_LAG is a *staleness SLA*, not a schedule interval. Snowflake auto-computes when to refresh, using incremental (change-tracking) logic where possible.

- `TARGET_LAG = '10 minutes'` → data must not be more than 10 min stale
- `TARGET_LAG = DOWNSTREAM` → refresh only when a downstream dynamic table needs fresh data (lazy evaluation)
- Dependency chains propagate TARGET_LAG automatically

**vs CREATE TASK:** Dynamic Table is *declarative/data-driven* (what result I want + how fresh). Task is *procedural/time-driven* (what SQL to run + when).

**Community DuckDB analog: `duckorch`**  
```sql
PRAGMA orch_create_dynamic_asset(
  'analytics.region_total',
  '5 minutes',
  'SELECT region, SUM(total) FROM analytics.daily GROUP BY region');
```
- Stored in `__orch__.assets` and `__orch__.asset_materializations`
- Snowflake dump migration: `duck-orch dynamic migrate-from-snowflake snowflake_dump.sql`
- Status: early, needs `cargo build` (not in community extension registry yet)

---

### 1e. ClickHouse Refreshable Materialized Views

Source: https://clickhouse.com/docs/materialized-view/refreshable-materialized-view

```sql
CREATE MATERIALIZED VIEW <name>
    REFRESH EVERY <interval>
    [ OFFSET <interval> ]
    [ RANDOMIZE FOR <interval> ]
    [ DEPENDS ON <other_mv> [, ...] ]
    [ APPEND ]
    ENGINE = <engine>
    ORDER BY (...)
  AS
    SELECT ...;
```

**Examples:**
```sql
-- Full replace every hour
CREATE MATERIALIZED VIEW hourly_agg
    REFRESH EVERY 1 HOUR
    ENGINE = MergeTree ORDER BY (ts, product_id)
AS SELECT toDate(ts) AS ts, product_id, sum(amount) FROM orders GROUP BY ALL;

-- Append-only snapshots every 10 seconds
CREATE MATERIALIZED VIEW events_snapshot
    REFRESH EVERY 10 SECOND APPEND TO events_snapshot
AS SELECT now() AS ts, uuid, sum(count) FROM events GROUP BY ALL;

-- Weekly with 2-day offset, depends on daily view
CREATE MATERIALIZED VIEW weekly_rollup
    REFRESH EVERY 1 WEEK OFFSET 2 DAY 3 HOUR
    DEPENDS ON daily_sales_summary
    ENGINE = MergeTree ORDER BY week_start
AS SELECT ...;
```

**Control:**
```sql
SYSTEM STOP VIEW my_mv;
SYSTEM START VIEW my_mv;
ALTER TABLE my_mv MODIFY REFRESH EVERY 30 SECONDS;
```

**Key design choices:**
- `RANDOMIZE FOR` adds jitter — prevents thundering herds when many views share same schedule
- `DEPENDS ON` — DAG between views (like Snowflake DOWNSTREAM)
- `APPEND` mode vs full replace — toggle between accumulate and snapshot
- Pure DDL — no separate "resume" step

---

### 1f. MySQL CREATE EVENT

Source: https://dev.mysql.com/doc/refman/8.0/en/create-event.html

```sql
CREATE [ DEFINER = user ] EVENT [ IF NOT EXISTS ] <name>
  ON SCHEDULE
    { AT <timestamp> [ + INTERVAL <interval> ... ]
    | EVERY <interval>
      [ STARTS <timestamp> [ + INTERVAL <interval> ] ]
      [ ENDS <timestamp> [ + INTERVAL <interval> ] ] }
  [ ON COMPLETION [ NOT ] PRESERVE ]
  [ ENABLE | DISABLE | DISABLE ON SLAVE ]
  [ COMMENT '<string>' ]
  DO <sql_statement>;
```

**Examples:**
```sql
-- One-shot: fire once in 1 hour
CREATE EVENT one_shot
  ON SCHEDULE AT CURRENT_TIMESTAMP + INTERVAL 1 HOUR
  DO UPDATE orders SET status = 'expired' WHERE age > 30;

-- Recurring every 6 weeks
CREATE EVENT weekly_clean
  ON SCHEDULE EVERY 6 WEEK
  DO DELETE FROM audit_log WHERE created_at < NOW() - INTERVAL 6 WEEK;

-- Recurring with explicit window
CREATE EVENT bounded_job
  ON SCHEDULE EVERY 1 DAY
    STARTS '2026-01-01 02:00:00'
    ENDS '2026-12-31 23:59:59'
  ON COMPLETION PRESERVE
  DO CALL generate_daily_report();
```

**Key design choices:**
- `ON COMPLETION PRESERVE` — one-shot events are NOT dropped after firing (useful for auditability)
- `STARTS` / `ENDS` — explicit activation window
- `DEFINER` — runs with the security context of a named user
- `ENABLE / DISABLE` — toggle without DROP
- Complex interval literals: `INTERVAL '2:10' MINUTE_SECOND`

---

### 1g. SQL Server Agent (T-SQL stored procedures)

No true DDL — jobs are managed through system stored procedures:

```sql
-- Step 1: create job
EXEC sp_add_job @job_name = 'DailyCleanup', @description = 'Purge old rows';

-- Step 2: add step(s)
EXEC sp_add_jobstep @job_name = 'DailyCleanup',
     @step_name = 'Delete old rows',
     @command = 'DELETE FROM events WHERE ts < DATEADD(DAY, -7, GETDATE())';

-- Step 3: attach schedule
EXEC sp_add_jobschedule @job_name = 'DailyCleanup',
     @name = 'NightlyAt2AM',
     @freq_type = 4,        -- daily
     @freq_interval = 1,
     @active_start_time = 20000;  -- 02:00:00

-- Step 4: register to a server
EXEC sp_add_jobserver @job_name = 'DailyCleanup';
```

**Verdict:** Procedural stored-proc API. Anti-pattern for quackapi — the multi-step, imperative nature is exactly what quackapi's single-DDL philosophy should improve upon.

---

## PART 2 — CATCH-ALL CREATE-* SWEEP

### 2a. DuckDB CREATE SECRET ✅ (native, production-ready)

Source: https://duckdb.org/docs/stable/sql/statements/create_secret

```sql
-- Temporary (default, in-memory for session lifetime)
CREATE SECRET my_s3_secret (
    TYPE s3,
    KEY_ID 'AKIAIOSFODNN7EXAMPLE',
    SECRET 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY',
    REGION 'us-east-1',
    SCOPE 's3://my-specific-bucket'   -- optional: scoped to path prefix
);

-- Persistent (survives session restart)
CREATE PERSISTENT SECRET my_azure_secret (
    TYPE azure,
    CONNECTION_STRING 'DefaultEndpointsProtocol=https;...'
);

-- OR REPLACE / IF NOT EXISTS
CREATE OR REPLACE SECRET gcs_cred (
    TYPE gcs,
    KEY_ID '...',
    SECRET '...'
);

-- credential_chain provider (auto-fetch from env/IAM/instance metadata)
CREATE SECRET (
    TYPE s3,
    PROVIDER credential_chain
);

-- HTTP Bearer token
CREATE SECRET api_token (
    TYPE http,
    TOKEN 'my-bearer-token'
);
```

**Supported secret types (10 total):** `s3`, `gcs`, `r2`, `azure`, `http`, `huggingface`, `iceberg`, `mysql`, `postgres`, `ducklake`, `quack`.

**Key design choices:**
- **SCOPE** — longest-prefix match when multiple secrets cover overlapping paths. Enables per-bucket vs global credentials.
- **TEMPORARY vs PERSISTENT** — explicit durability control.
- **PROVIDER** keyword — separates "how do I get the credentials" from "what type of service." `credential_chain` = auto-discovery (env vars → instance metadata → AWS profile).
- Security note: plaintext in DuckDB CLI history.

**quackapi mapping:** `CREATE SECRET` is adoptable as-is. Quackapi can treat secrets as named credential objects injected into endpoint context, scoped by route prefix (analogous to DuckDB's path scope).

---

### 2b. DuckDB CREATE MACRO ✅ (native, production-ready)

Source: https://duckdb.org/docs/current/sql/statements/create_macro

```sql
-- Scalar macro
CREATE MACRO add(a, b) AS a + b;
CREATE MACRO add_default(a, b := 5) AS a + b;
CREATE MACRO is_maximal(a INTEGER) AS a = 2^31 - 1;

-- Table macro (used in FROM clause — key pattern for quackapi endpoints)
CREATE MACRO search_products(q, min_price := 0) AS TABLE
    SELECT * FROM products
    WHERE name ILIKE '%' || q || '%'
      AND price >= min_price;

-- Usage
SELECT * FROM search_products('coffee', min_price := 5.00);

-- OR REPLACE, IF NOT EXISTS, TEMP
CREATE OR REPLACE MACRO route_handler(path) AS TABLE
    SELECT * FROM routes WHERE path_prefix = path;
```

**webmacro extension:** Load macros from remote URLs.
```sql
INSTALL webmacro FROM community;
LOAD webmacro;
SELECT load_macro_from_url('https://gist.github.com/.../my_macro.sql');
```

**quackapi mapping:** Table macros are *the* native DuckDB pattern for parameterized views / endpoint handlers. `CREATE MACRO search_products(q)` is structurally equivalent to `GET /search?q=...`. Quackapi could express routes as named table macros, with the router dispatching `FROM <macro_name>(params)`.

---

### 2c. DuckDB CREATE TYPE ✅ (native)

Source: https://duckdb.org/docs/current/sql/statements/create_type

```sql
-- ENUM
CREATE TYPE mood AS ENUM ('happy', 'sad', 'neutral');

-- STRUCT alias
CREATE TYPE address AS STRUCT(street VARCHAR, city VARCHAR, zip VARCHAR);

-- UNION type
CREATE TYPE animal AS UNION(cat VARCHAR, dog INT);

-- Simple alias
CREATE TYPE usd AS DECIMAL(18, 2);
```

**quackapi mapping:** `CREATE TYPE` enables domain modeling — define `OrderStatus`, `UserRole`, `MoneyAmount` once, use everywhere. For a SQL-native framework this is the analog of TypeScript `type` declarations or Pydantic models.

---

### 2d. DuckDB CREATE SEQUENCE ✅ (native)

Source: https://duckdb.org/docs/lts/sql/statements/create_sequence

```sql
CREATE SEQUENCE order_id_seq START 1000 INCREMENT 1;
SELECT nextval('order_id_seq');

-- Use as DEFAULT in table
CREATE TABLE orders (
    id BIGINT DEFAULT nextval('order_id_seq') PRIMARY KEY,
    ...
);
```

**quackapi mapping:** Useful for request IDs, correlation IDs, job IDs. Less interesting as DDL-to-steal since DuckDB already has it.

---

### 2e. Materialize CREATE SOURCE / CREATE SINK / CREATE CONNECTION ✅✅ STEAL

Sources:
- https://materialize.com/docs/sql/create-source/kafka/
- https://materialize.com/docs/sql/create-sink/
- https://materialize.com/docs/sql/create-source/postgres/

```sql
-- Step 1: Named reusable connection (credentials/network)
CREATE CONNECTION kafka_conn TO KAFKA (
    BROKER 'kafka:9092',
    SASL MECHANISMS = 'PLAIN',
    SASL USERNAME = 'user',
    SASL PASSWORD = SECRET kafka_password
);

-- Step 2: Source — brings external stream INTO the database
CREATE SOURCE page_views
    FROM KAFKA CONNECTION kafka_conn (TOPIC 'page_views')
    FORMAT JSON;

-- Postgres CDC source
CREATE SOURCE pg_source
    FROM POSTGRES CONNECTION pg_conn (PUBLICATION 'mz_source')
    FOR TABLES (orders, customers);

-- Step 3: Sink — pushes internal view OUT to external system
CREATE SINK orders_sink
    FROM orders_view
    INTO KAFKA CONNECTION kafka_conn (TOPIC 'orders_out')
    FORMAT JSON;
```

**Why brilliant:**
- **Three-tier separation:** CONNECTION (credentials/network) → SOURCE/SINK (what data + how) → MATERIALIZED VIEW (transform). Each tier can be independently ALTERED, DROPPED, or replaced.
- **CREATE CONNECTION is a named, reusable credentials object** — distinct from CREATE SECRET (which is just a key-value bag). A CONNECTION bundles credentials + endpoint + protocol into a single named entity that SOURCE and SINK reference.
- **Webhook-as-SINK pattern:** Any outbound integration (webhook, Kafka topic, S3 bucket) becomes a first-class DDL object.

**quackapi mapping:**
- `CREATE CONNECTION` → reusable named integration target (could power `CREATE WEBHOOK`, `CREATE NOTIFICATION`, `CREATE PUSH_TARGET`)
- `CREATE SOURCE` → inbound event stream or CDC feed an endpoint consumes
- `CREATE SINK` → outbound delivery to external system; webhook delivery becomes `CREATE SINK my_webhook FROM my_view INTO HTTP CONNECTION slack_conn (URL '...')`

---

### 2f. Postgres CREATE PUBLICATION / CREATE SUBSCRIPTION

Source: https://www.postgresql.org/docs/current/sql-createsubscription.html

```sql
-- Publisher side
CREATE PUBLICATION my_pub FOR TABLE orders, customers;

-- Subscriber side (on a different Postgres instance)
CREATE SUBSCRIPTION my_sub
    CONNECTION 'host=pub_host dbname=mydb user=rep password=...'
    PUBLICATION my_pub;
```

**quackapi mapping (limited):** The concept of PUBLICATION (declare what rows you broadcast) is interesting for *event subscriptions as DDL*. A quackapi `CREATE PUBLICATION orders_changed FOR TABLE orders` could mean "notify any registered webhook when orders rows change." The subscriber model is the webhook consumer.

**Red-team:** This is mostly a DBA replication primitive. The Materialize SOURCE/SINK model is cleaner for a web framework. SKIP as direct syntax, but steal the *concept* of declaring what events are emitted.

---

### 2g. Snowflake CREATE STREAM (CDC object)

```sql
CREATE OR REPLACE STREAM order_changes ON TABLE orders;

-- Consuming the stream in a task
SELECT * FROM order_changes WHERE METADATA$ACTION = 'INSERT';
```

**Concept:** A stream object that tracks DML changes (INSERT/UPDATE/DELETE) with metadata columns. The stream is consumed transactionally — reading empties the offset.

**quackapi mapping:** `CREATE STREAM my_stream ON TABLE orders` as a quackapi DDL would give you a named change feed. Extremely powerful for webhook triggers: "fire this endpoint every time a row in `orders` changes." Red-team: DuckDB has no native change-data CDC, so implementing this requires the `delta` extension or triggers. **Interesting but needs more infrastructure.**

---

### 2h. MySQL CREATE EVENT — One-Shot Pattern

**Steal:** The `AT <timestamp>` one-shot form + `ON COMPLETION PRESERVE` is elegant for deferred/scheduled jobs:

```sql
-- quackapi analog: run this job once in 30 minutes
CREATE JOB notify_user
  AT CURRENT_TIMESTAMP + INTERVAL 30 MINUTE
  ON COMPLETION PRESERVE
  AS SELECT send_webhook('https://...', (SELECT payload FROM pending WHERE id = 42));
```

ON COMPLETION PRESERVE prevents the job record from self-deleting, giving you an audit trail. Worth stealing in quackapi's CREATE JOB.

---

### 2i. Postgres/Redshift CREATE FOREIGN TABLE / CREATE SERVER

```sql
-- Postgres FDW pattern
CREATE SERVER remote_api
    FOREIGN DATA WRAPPER postgres_fdw
    OPTIONS (host 'api.example.com', port '5432', dbname 'store');

CREATE USER MAPPING FOR current_user
    SERVER remote_api
    OPTIONS (user 'apiuser', password 'secret');

CREATE FOREIGN TABLE remote_orders (
    id BIGINT, amount DECIMAL
) SERVER remote_api OPTIONS (table_name 'orders');
```

**quackapi steal:** `CREATE SERVER` as a DDL-level named external dependency. In quackapi terms: `CREATE SERVER upstream_db` gives you a named remote data source that endpoint macros can JOIN against. DuckDB already does this via `ATTACH` — the FDW pattern isn't needed. **SKIP** — DuckDB ATTACH is superior.

---

### 2j. DuckDB CREATE VIEW (as endpoint pattern)

```sql
-- Standard DuckDB
CREATE VIEW active_users AS
    SELECT id, name, email FROM users WHERE status = 'active';

-- Parameterized via MACRO (the actual endpoint pattern)
CREATE MACRO user_orders(user_id BIGINT) AS TABLE
    SELECT o.* FROM orders o WHERE o.user_id = user_id;
```

**Note:** A plain CREATE VIEW is not parameterized — it's a static read. The table MACRO form is what enables parameterized endpoint semantics. Worth calling out explicitly in quackapi docs.

---

### 2k. ClickHouse RANDOMIZE FOR (jitter)

```sql
REFRESH EVERY 1 HOUR RANDOMIZE FOR 5 MINUTE
```

**Steal:** `RANDOMIZE FOR` is one line of DDL that solves thundering-herd without any application code. When many CREATE JOBs share a schedule slot (e.g., `EVERY 1 HOUR`), jitter spreads them. quackapi should support `JITTER <interval>` in CREATE JOB / CREATE CRON.

---

## PART 3 — SYNTHESIS: quackapi DDL Recommendations

### CREATE JOB / CREATE CRON design

**Build ON the `cronjob` extension but add a DDL shell layer over it.**

```sql
-- Proposed quackapi DDL
CREATE JOB cleanup_old_logs
  SCHEDULE '0 3 * * *'           -- cron expression (5 or 6 fields)
  [ AT CURRENT_TIMESTAMP + INTERVAL 30 MINUTE ]  -- one-shot alternative
  [ JITTER INTERVAL '5 MINUTE' ]                 -- randomize for thundering-herd
  [ WHEN (SELECT COUNT(*) FROM pending_jobs) > 0 ]  -- conditional guard
  [ ON COMPLETION PRESERVE ]                     -- keep record after one-shot fires
  [ SUSPEND AFTER 5 FAILURES ]                   -- auto-disable on repeated failure
  [ TIMEOUT INTERVAL '10 MINUTE' ]
  AS
    DELETE FROM logs WHERE ts < NOW() - INTERVAL 7 DAY;
```

**Implementation:**
1. `INSTALL cronjob FROM community; LOAD cronjob;` at server startup.
2. Parser intercepts `CREATE JOB` DDL, extracts clauses, calls `cron(sql, schedule)` on the underlying extension.
3. `CREATE JOB` records go into a quackapi-managed `__quackapi__.jobs` table (metadata + status history) — filling the persistence gap that `cronjob` lacks.
4. On startup, quackapi re-registers all jobs from `__quackapi__.jobs` (solving the session-restart persistence problem).

**Alternative if `cronjob` remains too experimental:** Implement quackapi's own background thread in the server process (Python `threading.Thread` with a polling loop checking `next_run` timestamps).

### CREATE CRON alias

```sql
CREATE CRON every_hour AS 'EVERY 1 HOUR';  -- named schedule
-- then reference in job:
CREATE JOB my_job SCHEDULE every_hour AS ...;
```

---

## Summary Table: KEEP vs SKIP

| Source | Concept | Verdict | Priority |
|--------|---------|---------|----------|
| DuckDB `cronjob` | `cron(sql, expr)` + `cron_jobs()` | **KEEP — build on it** | P0 |
| Snowflake TASK `AFTER` | DAG dependencies | **KEEP — steal for CREATE JOB** | P1 |
| Snowflake TASK `WHEN` | Conditional execution guard | **KEEP** | P1 |
| Snowflake TASK `SUSPEND_AFTER_NUM_FAILURES` | Auto-disable on error | **KEEP** | P1 |
| ClickHouse `RANDOMIZE FOR` | Jitter / thundering herd | **KEEP as `JITTER`** | P1 |
| MySQL `ON COMPLETION PRESERVE` | One-shot audit trail | **KEEP** | P2 |
| MySQL `AT <timestamp>` | One-shot deferred fire | **KEEP** | P2 |
| MySQL `STARTS` / `ENDS` | Activation window | **KEEP** | P2 |
| Snowflake DYNAMIC TABLE `TARGET_LAG` | Staleness SLA vs schedule | **KEEP — future `CREATE ASSET`** | P2 |
| DuckDB `CREATE SECRET` | Named scoped credentials | **KEEP — native, adopt as-is** | P0 |
| DuckDB `CREATE MACRO` (table) | Parameterized view = endpoint | **KEEP — core quackapi primitive** | P0 |
| DuckDB `CREATE TYPE` | Domain type aliases + enums | **KEEP** | P1 |
| Materialize `CREATE CONNECTION` | Named reusable integration target | **KEEP — inform CREATE WEBHOOK** | P1 |
| Materialize `CREATE SINK` | Outbound delivery as DDL | **KEEP — webhook-as-sink pattern** | P1 |
| Snowflake `CREATE STREAM` | CDC change feed object | **KEEP concept, hard to implement** | P3 |
| Postgres `CREATE PUBLICATION` | Emit-what-changes declaration | **SKIP — Materialize SINK is cleaner** | — |
| Postgres FDW pattern | Named external server | **SKIP — DuckDB ATTACH supersedes** | — |
| SQL Server `sp_add_job` | Procedural job API | **SKIP — anti-pattern** | — |
| DuckDB `CREATE SEQUENCE` | Auto-increment IDs | **SKIP — already native** | — |
| `duckorch` `PRAGMA orch_create_dynamic_asset` | Asset orchestration | **WATCH — not production-ready** | P3 |

---

## Sources

- [cronjob – DuckDB Community Extensions](https://duckdb.org/community_extensions/extensions/cronjob)
- [GitHub - quackscience/duckdb-extension-cronjob](https://github.com/quackscience/duckdb-extension-cronjob)
- [DuckDB Community Extensions List](https://duckdb.org/community_extensions/list_of_extensions)
- [CREATE SECRET Statement – DuckDB](https://duckdb.org/docs/stable/sql/statements/create_secret)
- [Secrets Manager – DuckDB](https://duckdb.org/docs/current/configuration/secrets_manager)
- [CREATE MACRO Statement – DuckDB](https://duckdb.org/docs/current/sql/statements/create_macro)
- [CREATE SEQUENCE Statement – DuckDB](https://duckdb.org/docs/lts/sql/statements/create_sequence)
- [CREATE TYPE Statement – DuckDB](https://duckdb.org/docs/current/sql/statements/create_type)
- [webmacro – DuckDB Community Extensions](https://duckdb.org/community_extensions/extensions/webmacro)
- [CREATE TASK – Snowflake Documentation](https://docs.snowflake.com/en/sql-reference/sql/create-task)
- [CREATE DYNAMIC TABLE – Snowflake Documentation](https://docs.snowflake.com/en/sql-reference/sql/create-dynamic-table)
- [Dynamic tables compared to streams and tasks – Snowflake](https://docs.snowflake.com/en/user-guide/dynamic-tables-comparison)
- [Refreshable Materialized View – ClickHouse Docs](https://clickhouse.com/docs/materialized-view/refreshable-materialized-view)
- [CREATE EVENT – MySQL 8.0 Reference Manual](https://dev.mysql.com/doc/refman/8.0/en/create-event.html)
- [sp_add_job (Transact-SQL) – Microsoft Learn](https://learn.microsoft.com/en-us/sql/relational-databases/system-stored-procedures/sp-add-job-transact-sql)
- [CREATE SOURCE: Kafka – Materialize Documentation](https://materialize.com/docs/self-managed/v25.1/sql/create-source/kafka/)
- [CREATE SINK – Materialize Documentation](https://materialize.com/docs/sql/create-sink/)
- [CREATE SUBSCRIPTION – PostgreSQL Documentation](https://www.postgresql.org/docs/current/sql-createsubscription.html)
- [Introduction to streams – Snowflake Documentation](https://docs.snowflake.com/en/user-guide/streams-intro)
- [GitHub - nkwork9999/duck-orch](https://github.com/nkwork9999/duck-orch)
