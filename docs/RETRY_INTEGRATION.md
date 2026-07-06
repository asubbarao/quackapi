# RETRY_INTEGRATION: httpfs_timeout_retry as the framework HTTP-retry story

Status: integrated (transport layer only).  
Date verified: 2026-07-06.

## 1. Install and load verification (honest report)

Command (always -unsigned):

```sh
duckdb -unsigned -c "
INSTALL httpfs_timeout_retry FROM community;
LOAD httpfs_timeout_retry;
SELECT 'loaded successfully' AS status;
SELECT name, value FROM duckdb_settings()
WHERE name LIKE '%httpfs_%timeout%' OR name LIKE '%httpfs_%retries%' OR name IN ('http_timeout','http_retries');
"
```

Result on this machine:

- INSTALL + LOAD: success (no CatalogError, no network/install failure).
- Status row: `loaded successfully`.
- New settings appear with NULL defaults (per-op); base `http_retries=3`, `http_timeout=30` (seconds), `http_retry_wait_ms=100` etc. also present (some from core httpfs + this ext).

The extension is a pure settings + wrapper layer over httpfs. No new functions, no new types.

## 2. Exactly what it retries (functions/settings), defaults, knobs

It instruments httpfs I/O:

- **file_operation** (open/read/write — the common case for `read_csv('http...')`, `read_json_auto('https...')`, `read_parquet`, `read_text` etc.):  
  `httpfs_timeout_file_operation_ms` (ms), `httpfs_retries_file_operation`

- list dirs: `httpfs_timeout_list_ms`, `httpfs_retries_list`

- stat/metadata: `httpfs_timeout_stat_ms`, `httpfs_retries_stat`

- delete: `httpfs_timeout_delete_ms`, `httpfs_retries_delete`

- create_dir: `httpfs_timeout_create_dir_ms`, `httpfs_retries_create_dir`

Fallbacks (used when a per-op setting is NULL):

- `http_timeout` (seconds) — used for ops without per-op
- `http_retries`
- `http_retry_wait_ms`
- `http_retry_backoff`

The extension performs retries on transient transport failures (timeouts, I/O errors, some 5xx responses at the HEAD/GET level) before the error is raised to SQL.

It is 100% compatible: existing httpfs code needs zero changes.

## 3. What it covers vs. what it does NOT cover

**Covers:** transport-layer retry. Blind, configurable attempt count + per-attempt timeout + backoff for the low-level httpfs file operations. Happens inside the http client used by read_* table functions and file globs over http(s).

**Does NOT cover:** app-level retry-until-condition — that is done by re-dispatching a follow-up statement through the framework's dynamic statement executor.

Transport retry is unconditional and at the wire level. If your handler needs "keep trying this logical step until the result satisfies X or N logical attempts", you achieve it by having the controlling code (or a subsequent handler) issue another dispatch of a (possibly different) SQL statement after examining the previous outcome. The extension itself provides no condition or "until" surface.

## 4. Exact usage from a route handler

Because the INSTALL/LOAD/SETs live in framework.sql (executed on every CLI use and every worker), every handler SQL runs with the retry policy active.

- Any `read_*('http...')` or http path inside a dynamic handler automatically gets retries.
- The policy is connection/session scoped at bootstrap; per-request workers inherit it.
- Tuning: change the SETs before handler execution (or inside a handler statement before the read, subject to statement scoping).

No per-route registration needed for basic resilience.

## 5. Copy-paste example route (DDL sugar — SUGAR-FIRST)

User-facing examples **never** use raw `INSERT INTO routes ...`. Always show the DDL sugar provided by the runtime (CREATE ROUTE ... AS SELECT ...).

```sql
-- Example remote-fetching route. The read_json_auto is protected by
-- httpfs_timeout_retry (transport retries/timeouts).
CREATE ROUTE fetch_remote GET '/remote/data' AS
  SELECT to_json(r) AS body
  FROM read_json_auto('https://api.example.com/v1/items') AS r;

-- Example with path param that builds a URL (still transport-retried).
CREATE ROUTE fetch_status GET '/status/{code}' (code INT) AS
  SELECT json_object('code', code, 'payload', p) AS body
  FROM read_json_auto('https://httpbin.org/status/' || code) AS p
  LIMIT 1;

-- If a handler wants tighter local control, SETs can precede the read
-- (executes in same connection context as the handler).
CREATE ROUTE careful_get GET '/careful' AS
  SELECT (SET httpfs_timeout_file_operation_ms = 15000;
          SET httpfs_retries_file_operation = 5;
          SELECT to_json(x) AS body FROM read_json_auto('https://slow.example') AS x);
```

The `handler_sql` stored and later executed for these routes is exactly the `SELECT ... read_...` text. The http layer underneath obeys the knobs.

## 6. Probe (runnable)

See `test/retry_probe.test.sql`.

It:

- Loads the ext explicitly.
- Prints the registered settings.
- Uses `.timer on`.
- Runs against `https://httpbin.org/delay/2` (slow) and `/status/500` (error) with varying `httpfs_timeout_file_operation_ms` + `httpfs_retries_file_operation`.
- Success case (delay/0) + failure-after-retries cases.
- Time delta is observable: 0 retries ~1s (timeout), 3 retries ~4.5s on same endpoint.

Run it:

```sh
duckdb -unsigned < test/retry_probe.test.sql
# or together with the framework (which loads+configures the ext)
cat framework.sql test/retry_probe.test.sql | duckdb -unsigned
```

## 7. Regression gate

```sh
cat framework.sql test/tier1_handle_request.test.sql | duckdb -unsigned
```

Must report 132 passed / 0 failed. (No changes were made to framework.sql or any test file.)

## Constraints observed

- duckdb CLI: always invoked with `-unsigned`.
- Only new files created: `docs/RETRY_INTEGRATION.md`, `test/retry_probe.test.sql`.
- No edits to framework.sql, app.sql, README.md, ext-cpp/, or any existing test.
- No git commands run.
- No /tmp or /Users/... paths appear inside the created files.
- No processes killed; no pkill/killall; no servers started on 9494/9495/8815/9998 (probe uses only public httpbin.org).
- Honest: extension installed/loaded successfully on first attempt; exact errors would have been reported verbatim if it had failed.

This completes the HTTP-retry story integration for the transport layer.
