# DDL reference — every CREATE noun

Grammar below is taken from the parser extensions in `src/`. Examples were run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

Drop forms exist for each noun unless noted.

---

## 1. CREATE ROUTE

```sql
CREATE [OR REPLACE] ROUTE <name> <METHOD> '<pattern>'
  [STATUS <n>]
  [REQUIRE <auth>]
  [GROUP <group> | IN GROUP <group>]
  [BODY SCHEMA '<json-schema>']
  [PARAM <name> [<type>] [HEADER|COOKIE|QUERY [wire-name]]
         [DEFAULT <lit>] [GE|GT|LE|LT <n>] [MIN_LENGTH|MAX_LENGTH <n>] …]
  AS <select-or-dml>;

DROP ROUTE <name>;
```

| Piece | Rules |
|-------|--------|
| **METHOD** | `GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD` |
| **pattern** | Quoted. Must start with `/` unless the route is in a GROUP (relative join allowed). Captures: `:id` or `{id}` → `$id` |
| **STATUS** | Integer 100–599. Default 200 |
| **REQUIRE** | Auth scheme name (checked at request time) |
| **GROUP / IN GROUP** | Join group prefix + inherit auth/tags |
| **BODY SCHEMA** | Quoted JSON Schema string; may appear before or after PARAM |
| **PARAM** | Zero or more. Types: INTEGER/INT, BIGINT, VARCHAR/TEXT/STRING, BOOLEAN/BOOL, DOUBLE, FLOAT/REAL, HUGEINT, UBIGINT, UINTEGER |
| **AS** | Any SQL returning a result; validated at CREATE time |

**Example:**

```sql
CREATE ROUTE item GET '/items/:id'
  PARAM pretty BOOLEAN DEFAULT false
  AS
SELECT $id::INTEGER AS id, $pretty AS pretty;
```

**Response control columns:** `html`, `text`, `location`, `set_cookie` / `set-cookie` — see [headers guide](../guide/headers-cookies-redirects.md).

---

## 2. CREATE AUTH

```sql
CREATE [OR REPLACE] AUTH <name> AS API_KEY [ ( HEADER '<header-name>' ) ];

CREATE [OR REPLACE] AUTH <name> AS JWT (
  SECRET '<secret>'
  [, ALGORITHM HS256]
);

DROP AUTH <name>;
```

| Kind | Default header | Notes |
|------|----------------|-------|
| `API_KEY` | `X-API-Key` | Keys added via `quackapi_add_api_key` (SHA-256 stored) |
| `JWT` | `Authorization` (Bearer) | **HS256 only** |

**Example:**

```sql
CREATE AUTH site AS API_KEY;
CREATE AUTH jwt_auth AS JWT ( SECRET 'conformance-secret' );
```

---

## 3. CREATE GROUP / CREATE API GROUP

```sql
CREATE [OR REPLACE] [API] GROUP <name>
  WITH (
    prefix='/absolute/path'
    [, auth=<auth-name> | require=<auth-name>]
    [, tags='csv,tags']
  );

-- Keyword form (no WITH):
CREATE [OR REPLACE] API GROUP <name>
  PREFIX '/p'
  [TAGS 't']
  [REQUIRE <auth>];

DROP [API] GROUP <name>;
```

`CREATE GROUP` and `CREATE API GROUP` are synonyms. `auth=` and `require=` are synonyms.

**Example:**

```sql
CREATE GROUP v1 WITH (prefix='/api/v1', auth=api, tags='items,v1');
CREATE ROUTE items_list GET '/items' GROUP v1 AS SELECT 1 AS id;
```

---

## 4. CREATE API FOR TABLE

```sql
CREATE [OR REPLACE] API FOR TABLE <table>
  [AT '<base_path>']
  [KEY '<column>'];
```

| Clause | Default | Constraint |
|--------|---------|------------|
| table | — | Bare id or `"quoted""ident"` |
| `AT` | `'/<table>'` | Quoted path starting with `/` |
| `KEY` | `'id'` | Bare identifier → path param |

Expands to:

- `GET <base>` → `SELECT * FROM <table>`
- `GET <base>/:<key>` → `SELECT * FROM <table> WHERE <key> = $<key>`

Read-only. Generated names: `<table>_list`, `<table>_get`.

**Example:**

```sql
CREATE API FOR TABLE documents AT '/api/documents' KEY 'doc_id';
```

---

## 5. CREATE QUEUE

```sql
CREATE [OR REPLACE] QUEUE <name>
  [WITH (
     max_attempts=<n>,                 -- default 3; range 1..1000
     visibility_timeout='30s'|30,      -- s/m/h suffix or seconds
     backoff_base_seconds=<n>          -- default 2; 0 = immediate
  )];

DROP QUEUE <name>;
```

Queue name: `[A-Za-z0-9_-]+`.

Creates durable table **`quackapi_jobs`** on first use.

**Example:**

```sql
CREATE QUEUE emails WITH (max_attempts=5, visibility_timeout='30s');
```

---

## 6. CREATE STREAM

```sql
CREATE [OR REPLACE] STREAM <name> GET '<path>'
  [WITH (interval='1s'|2|'250ms')]
  AS <select>;

DROP STREAM <name>;
```

| Rule | Detail |
|------|--------|
| Method | **GET only** (SSE) |
| Path | Must start with `/` |
| interval | Polling period for long-lived streams |
| WS / WEBSOCKET / WSS | **Rejected** with an explicit error |

**Example:**

```sql
CREATE STREAM ticks GET '/ticks' AS
SELECT i AS id, 'tick' AS msg FROM range(3) t(i);
```

---

## 7. CREATE ROW ACCESS POLICY

```sql
CREATE [OR REPLACE] ROW ACCESS POLICY <name>
  AS (<col> [<type>] [, ...])
  RETURNS BOOLEAN
  USING (<boolean expression>);

ALTER TABLE <table>
  ADD ROW ACCESS POLICY <name> ON (<col> [, ...]);

DROP ROW ACCESS POLICY <name>;
```

Expression may reference policy columns and `$claims_*`.

**Example:**

```sql
CREATE ROW ACCESS POLICY tenant_isolation
  AS (tenant_id VARCHAR) RETURNS BOOLEAN
  USING (tenant_id = $claims_tenant OR $claims_role = 'admin');

ALTER TABLE pol_orders
  ADD ROW ACCESS POLICY tenant_isolation ON (tenant_id);
```

---

## 8. CREATE MASKING POLICY

```sql
CREATE [OR REPLACE] MASKING POLICY <name>
  ON <type>
  USING (<expression>);

ALTER TABLE <table>
  MODIFY COLUMN <col> SET MASKING POLICY <name>;

DROP MASKING POLICY <name>;
```

Inside `USING`, **`val`** is the original column value. Claims bind as `$claims_*`.

**Example:**

```sql
CREATE MASKING POLICY mask_email ON VARCHAR
  USING (CASE WHEN $claims_role = 'admin' THEN val ELSE '***' END);

ALTER TABLE pol_users
  MODIFY COLUMN email SET MASKING POLICY mask_email;
```

---

## Lifecycle reminder

| Object | Survives process restart? |
|--------|---------------------------|
| Routes, auth, groups, queues (options), streams, policies | **No** — re-run DDL after reopen (instance registry) |
| `quackapi_jobs` rows | **Yes** — normal table in your `.db` file |
| Table data | **Yes** |
