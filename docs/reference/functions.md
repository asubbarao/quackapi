# Function reference — every `quackapi_*`

Authoritative list from [FEATURE_STATUS §1.4](../FEATURE_STATUS.md) (live registry). Signatures from `src/` and versioned tests. One-line examples were run against `build/release/duckdb -unsigned`.

---

## Server lifecycle

### `quackapi_serve([port], host := …, static_dir := …, cors_origins := …, memory_limit := …, http_client := …)`

| | |
|--|--|
| **Kind** | Table function |
| **Args** | `port INTEGER` optional (default in implementation if omitted — prefer passing explicitly, e.g. `8000`) |
| **Named** | `host VARCHAR` (default `127.0.0.1`), `static_dir VARCHAR`, `cors_origins VARCHAR`, `memory_limit VARCHAR`, `http_client VARCHAR` (`auto`\|`curl`\|`httplib`), plus batteries knobs (`log_level`, `compression`, …) |
| **Returns** | `listen_url VARCHAR` |

```sql
SELECT * FROM quackapi_serve(8000);
-- http://127.0.0.1:8000

SELECT * FROM quackapi_serve(
  8000,
  host := '127.0.0.1',
  static_dir := './static',
  cors_origins := '*',
  memory_limit := '4GB',
  http_client := 'auto'   -- prefer curl_httpfs; fall back to httplib
);
```

**Memory limit precedence:** named param → `SET quackapi_memory_limit` → leave non-default DuckDB `memory_limit` alone → else safe default **256MB**.

**Outbound HTTP client:** default `auto` INSTALL/LOADs community `curl_httpfs` and sets
`httpfs_client_implementation='curl'` (connection pool, HTTP/2, async). If unavailable
(Windows/WASM/offline), logs `quackapi.http_client=httplib reason=curl_httpfs_unavailable`
and continues. Override with `http_client := 'httplib'` or `SET quackapi_http_client`.
Inbound server remains httplib. See [curl_httpfs.md](../curl_httpfs.md).

---

### `quackapi_stop([port])`

| | |
|--|--|
| **Kind** | Table function |
| **Args** | `port INTEGER` optional — omit to stop **all** servers |
| **Returns** | `status VARCHAR` |

```sql
SELECT * FROM quackapi_stop(8000);
-- Stopped quackapi server on port 8000

SELECT * FROM quackapi_stop();
-- Stopped all quackapi servers
```

---

### `quackapi_servers()`

| | |
|--|--|
| **Kind** | Table function |
| **Returns** | `host`, `port`, `listen_url`, `http_client` (`curl` or `httplib`) |

```sql
SELECT * FROM quackapi_servers();
```

---

## Registry inspection

### `quackapi_routes()`

| | |
|--|--|
| **Returns** | `name`, `method`, `pattern`, `status`, `handler`, `require_auth`, `group_name`, `tags` |

```sql
SELECT name, method, pattern FROM quackapi_routes();
```

---

### `quackapi_auths()`

| | |
|--|--|
| **Returns** | `name`, `kind`, `header` — **never** secrets or key hashes |

```sql
SELECT name, kind, header FROM quackapi_auths();
-- site | API_KEY | X-API-Key
```

---

### `quackapi_groups()`

| | |
|--|--|
| **Returns** | `name`, `prefix`, `require_auth`, `tags`, `members` |

```sql
SELECT name, prefix, members FROM quackapi_groups();
```

---

### `quackapi_queues()`

| | |
|--|--|
| **Returns** | `name`, `depth`, `in_flight`, `dead`, `max_attempts`, `visibility_timeout_sec`, `backoff_base_sec` |

```sql
SELECT name, depth, in_flight, dead FROM quackapi_queues();
```

---

### `quackapi_streams()`

| | |
|--|--|
| **Returns** | `name`, `method`, `pattern`, `transport`, `interval_ms`, `handler` |

```sql
SELECT name, pattern, transport, interval_ms FROM quackapi_streams();
```

---

### `quackapi_policies()`

| | |
|--|--|
| **Returns** | `name`, `kind`, `signature`, `expression`, `bound_table`, `bound_columns` |

```sql
SELECT name, kind, bound_table FROM quackapi_policies();
```

---

## Auth helpers

### `quackapi_add_api_key(auth_name, raw_key, subject)`

| | |
|--|--|
| **Kind** | Table function |
| **Args** | three `VARCHAR` |
| **Returns** | `subject VARCHAR` |
| **Side effect** | Stores **SHA-256** of `raw_key` under the API_KEY scheme |

```sql
SELECT * FROM quackapi_add_api_key('site', 'k-secret', 'alice');
-- alice
```

Errors if scheme missing or not `API_KEY`.

---

### `quackapi_verify_auth(scheme, auth_string)`

| | |
|--|--|
| **Kind** | Scalar → struct |
| **Args** | `scheme VARCHAR`, `auth_string VARCHAR` |
| **Returns** | struct with at least `ok BOOLEAN`, `status INTEGER`, `claims_json VARCHAR` |

```sql
SELECT (quackapi_verify_auth('site', 'k-secret')).ok;     -- true
SELECT (quackapi_verify_auth('site', 'wrong')).status;    -- 401
```

---

### `quackapi_authentication(session_id, auth_string, token)`

| | |
|--|--|
| **Kind** | Scalar |
| **Args** | three `VARCHAR` |
| **Returns** | `BOOLEAN` |

True if `auth_string` equals `token` (timing-safe) **or** matches any registered auth scheme.

```sql
SELECT quackapi_authentication('sess', 'mytoken', 'mytoken');  -- true
SELECT quackapi_authentication('sess', 'k-secret', 'other');   -- true if k-secret is a registered API key
```

---

### `quackapi_authorization(session_id, query)`

| | |
|--|--|
| **Kind** | Scalar |
| **Args** | two `VARCHAR` |
| **Returns** | `VARCHAR` (pass-through of `query`) |

```sql
SELECT quackapi_authorization('sess', 'SELECT 1');  -- SELECT 1
```

---

## Queue

### `quackapi_enqueue(queue, payload [, max_attempts])`

| | |
|--|--|
| **Kind** | Scalar |
| **Args** | `queue VARCHAR`, `payload VARCHAR` or `JSON`, optional `max_attempts INTEGER` |
| **Returns** | `job_id BIGINT` |

```sql
SELECT quackapi_enqueue('default', '{"task":"email"}');
-- 1
```

---

### `quackapi_dequeue(queue [, n])`

| | |
|--|--|
| **Kind** | Table function |
| **Args** | `queue VARCHAR`, optional `n INTEGER` (default 1, max 1000) |
| **Returns** | `id`, `queue`, `payload`, `status`, `attempts`, `max_attempts`, `visible_at`, `last_error` |

```sql
SELECT id, payload, status FROM quackapi_dequeue('default', 10);
```

---

### `quackapi_ack(queue, job_id)`

| | |
|--|--|
| **Kind** | Scalar |
| **Args** | `queue VARCHAR`, `job_id BIGINT` |
| **Returns** | `BOOLEAN` — true if job was `running` and marked `done` |

```sql
SELECT quackapi_ack('default', 1);  -- true
```

---

### `quackapi_nack(queue, job_id [, requeue [, error]])`

| | |
|--|--|
| **Kind** | Scalar |
| **Args** | `queue VARCHAR`, `job_id BIGINT`, optional `requeue BOOLEAN` (default true), optional `error VARCHAR` |
| **Returns** | `VARCHAR` new status (`pending` or `dead`) |

```sql
SELECT quackapi_nack('default', 1, true, 'try_again');  -- pending or dead
SELECT quackapi_nack('default', 1, false, 'no_retry');  -- dead
```

---

## Durable table

### `quackapi_jobs`

Created on first `CREATE QUEUE`. Ordinary catalog table:

| Column | Role |
|--------|------|
| `id` | Job id |
| `queue` | Queue name |
| `payload` | VARCHAR JSON text |
| `status` | `pending` / `running` / `done` / `dead` |
| `attempts`, `max_attempts` | Retry bookkeeping |
| `visible_at` | Lease / backoff timestamp |
| `last_error` | Last nack message |

```sql
SELECT id, payload, status FROM quackapi_jobs WHERE status = 'done';
```

---

## Diagnostics

### `quackapi_http_util_name()`

| | |
|--|--|
| **Kind** | Scalar |
| **Returns** | `VARCHAR` name of the active outbound HTTP util |

```sql
SELECT quackapi_http_util_name();
-- Built-In
-- (becomes MultiCurl after LOAD curl_httpfs, if installed)
```

Outbound HTTPS for handlers that call `read_text` / httpfs uses DuckDB’s shared HTTP stack — quackapi does not link its own curl.

---

## Settings

| Setting | Meaning |
|---------|---------|
| `SET quackapi_cors_origins = '*' \| 'https://a,https://b'` | CORS allow list; empty = off |
| `SET quackapi_memory_limit = '4GB' \| '512MB' \| …` | Serve memory preference when named param omitted |
| `SET quackapi_http_client = 'auto' \| 'curl' \| 'httplib'` | Outbound httpfs client preference (default `auto` → curl_httpfs) |

```sql
SET quackapi_cors_origins = '*';
SET quackapi_memory_limit = '4GB';
SET quackapi_http_client = 'auto';
```

---

## Built-in HTTP paths (not functions)

Served automatically when any `quackapi_serve` is listening:

| Path | Role |
|------|------|
| `GET /openapi.json` | OpenAPI 3.1 |
| `GET /docs` | Swagger UI |
| `GET /redoc` | ReDoc |

See [OpenAPI guide](../guide/openapi.md).
