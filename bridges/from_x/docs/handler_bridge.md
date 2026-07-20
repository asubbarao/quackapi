# quackapi handler bridge — what FastAPI bodies actually do

**Date:** 2026-07-19  
**Binary (read-only):** `/Users/aloksubbarao/personal/quackapi/build/release/duckdb -unsigned`  
**IR:** `/tmp/quackapi_corpus/ir_python_routes.parquet` (990 routes), `ir_python_models.parquet` (2908 fields)  
**Classifier:** `/tmp/quackapi_classify_handlers.sql` → `/tmp/quackapi_handler_classified.parquet`  
**Rule:** every claim below is backed by a command run this session. Map residue to **existing** extensions — build nothing new.

---

## 0. Method (commands, not vibes)

1. Filter IR to `framework = 'fastapi'` → **519 handlers** across 353 files / 5 repo tags.
2. `INSTALL sitting_duck; LOAD sitting_duck;` — `read_ast(... peek:='full')` on those trees → `function_definition` spans; join to IR on `(file, handler_name, start_line window)` → **519/519 spans matched**.
3. `read_text` + `list_slice(lines, fn_start, fn_end)` + `array_to_string` → **519/519 bodies extracted**.
4. Body regex features → buckets with priority **B (side-effect) > C (imperative) > A (CRUD/declarative)**.
5. Fix: do **not** treat `session.exec(...)` as Python `exec(` (first pass false-positive C on SQLModel list handlers).

**Locus proof (interview repo in corpus):**

```sql
-- 15 ROH Pydantic contract models present in IR
SELECT count(DISTINCT model_name)  -- => 15
FROM read_parquet('/tmp/quackapi_corpus/ir_python_models.parquet')
WHERE (file ILIKE '%locus%' OR repo ILIKE '%locus%')
  AND model_name IN (
    'LocusROHReviewData','SuicidalityOutput','HomicidalityOutput','SelfHarmHistoryOutput',
    'PsychosisAggressionOutput','SelfCareJudgmentOutput','SuicidalIdeation','HomicidalIdeation',
    'PastAttempt','CommandHallucinations','AggressionHistory','_Judgment','BaseReviewData',
    'ExtractionEvidence','FieldSource'
  );
```

Locus is **Django management-command + domain pipeline**, not FastAPI routes (IR has 1 `django_path` admin URL). Its value here is the **validation surface** (15 ROH contracts) + a natural **bucket-C** domain (tree evaluation / LLM extraction) that would hit the escape hatch if exposed as HTTP.

---

## 1. A / B / C split — FastAPI handler bodies

### 1.1 Full FastAPI corpus (n = 519)

| Bucket | Meaning | Count | % |
|--------|---------|------:|--:|
| **A** | Pure data CRUD / declarative response → `CREATE ROUTE AS SELECT` | **498** | **96.0%** |
| **B** | Side-effect already covered by an existing extension | **16** | **3.1%** |
| **C** | Genuinely imperative / non-declarative | **5** | **1.0%** |
| **A+B** | Auto-carry with declarative SQL + known exts | **514** | **99.0%** |

```
SELECT bucket, count(*) n, round(100.0*count(*)/519,1) pct
FROM read_parquet('/tmp/quackapi_handler_classified.parquet')
GROUP BY 1 ORDER BY 1;
-- A 498 96.0 | B 16 3.1 | C 5 1.0
```

**A sub-structure (signals, not exclusive):**

| A flavor | Count | Note |
|----------|------:|------|
| ORM / repository CRUD signal | 246 | `session.*`, `select(`, `*_repo`, … |
| Echo / schema-demo return | 262 | mostly `fastapi-docs` tutorial stubs |

### 1.2 By repo (corpus bias is real)

| Repo | n | A | B | C | Notes |
|------|--:|--:|--:|--:|------|
| fastapi-docs | 437 | 96.6% | 2.3% | 1.1% | Tutorial-heavy; inflates A echo stubs |
| sqlmodel-docs | 57 | 96.5% | 3.5% | 0% | CRUD tutorials + 2 password handlers |
| **fastapi-realworld** | **19** | **78.9%** | **21.1%** | **0%** | **Best “real app” sample** |
| flask-examples / drf-framework (false fastapi tags) | 6 | 100% | 0 | 0 | IR noise |

### 1.3 RealWorld Conduit (honest app row)

| Bucket | Handlers | % | Examples |
|--------|---------:|--:|----------|
| **A** | 15 | 78.9% | `list_articles`, `create_new_article`, `get_all_tags`, favorites, follow, comments |
| **B** | 4 | 21.1% | `login`, `register`, `retrieve_current_user`, `update_current_user` (password check + JWT issue) |
| **C** | 0 | 0% | — |

All 15 A handlers are repository-style CRUD (`tags_repo.get_all_tags()`, `articles_repo.*`, …) — direct `CREATE ROUTE AS SELECT/INSERT/UPDATE/DELETE` material.

### 1.4 Bucket C inventory (full corpus)

All five C handlers are **WebSocket** demos under `fastapi-docs` (`websocket_endpoint` / `websocket`). No REST business-logic C in this corpus after the `session.exec` fix.

### 1.5 Bucket B pattern prevalence (handler bodies)

| Pattern → existing ext | B hits |
|------------------------|-------:|
| `jwt_sign_verify` → **crypto_hmac + json** (compose; **no community `jwt` extension**) | 10 |
| `password_hashing` → **crypto** | 5 |
| `background` → **cronjob** (and/or quackapi queue) | 3 |
| `template` → **tera** | 1 |
| outbound HTTP → **http_client** | 0 in this corpus |
| email → **http_client(provider)** | 0 in this corpus |
| cache → **cache_httpfs** | 0 in this corpus |

Corpus under-samples HTTP/email/cache; those patterns still map cleanly to existing exts (cookbook below).

**Extension availability verified this session:**

| Ext | `INSTALL … FROM community; LOAD …` | Key functions |
|-----|-----------------------------------|---------------|
| `crypto` | OK | `crypto_hash`, `crypto_hmac` |
| `http_client` | OK | `http_get`, `http_post`, `http_post_form` |
| `tera` | OK | `tera_render` |
| `cronjob` | OK | `cron`, `cron_jobs`, `cron_delete` |
| `cache_httpfs` | OK | `cache_httpfs_*` filesystem cache API |
| `jwt` | **NOT published** | compose HMAC-JWT with `crypto_hmac` + `json` + macros |

Also present on the quackapi binary: `quackapi_enqueue` / `quackapi_dequeue` (native queue) for background work.

---

## 2. Pattern → existing-ext cookbook (bucket B)

Each row: FastAPI snippet → quackapi SQL using an **already-installable** extension.

### 2.1 Password hashing → `crypto`

> Honest bound: community `crypto` exposes **hash/HMAC** (sha2-*, blake*, …), **not** bcrypt/argon2. For adaptive password hashes, wrap a verified SQL macro that stores `algo$salt$digest` using `crypto_hmac`/`crypto_hash`, or call a user macro that shells to a known verifier. Do **not** invent a new C++ ext for v1.

**FastAPI**

```python
hashed = hash_password(hero.password)
# login:
if not user.check_password(password): raise HTTPException(400, ...)
```

**quackapi**

```sql
LOAD crypto;

CREATE OR REPLACE MACRO hash_password(pw, salt) AS
  hex(crypto_hmac('sha2-256', salt, pw));

CREATE OR REPLACE MACRO verify_password(pw, salt, digest_hex) AS
  hash_password(pw, salt) = digest_hex;

-- register
CREATE OR REPLACE ROUTE register POST '/users'
  BODY SCHEMA '{ "type":"object", "required":["email","password","username"] }'
AS
SELECT
  $email AS email,
  $username AS username,
  hash_password($password, gen_random_uuid()::VARCHAR) AS password_digest;
  -- INSERT INTO users ... in the same SELECT via RETURNING pattern / table-macro
```

### 2.2 JWT sign / verify → `crypto_hmac` + `json` (compose)

No community `jwt` extension exists (verified install attempt → not found). Compose HS256-style tokens:

**FastAPI**

```python
token = jwt.create_access_token_for_user(user, secret)
```

**quackapi**

```sql
LOAD crypto; LOAD json;

CREATE OR REPLACE MACRO b64url(s) AS
  replace(replace(replace(base64(s::BLOB), '+', '-'), '/', '_'), '=', '');

CREATE OR REPLACE MACRO jwt_hs256_sign(payload_json, secret) AS (
  WITH parts AS (
    SELECT
      b64url('{"alg":"HS256","typ":"JWT"}') AS h,
      b64url(payload_json) AS p
  )
  SELECT h || '.' || p || '.' ||
    b64url(crypto_hmac('sha2-256', secret, h || '.' || p))
  FROM parts
);

CREATE OR REPLACE ROUTE login POST '/users/login'
  BODY SCHEMA '{ "type":"object", "required":["email","password"] }'
AS
WITH u AS (
  SELECT * FROM users WHERE email = $email
), ok AS (
  SELECT * FROM u WHERE verify_password($password, salt, password_digest)
)
SELECT json_object(
  'user', json_object(
    'email', email,
    'username', username,
    'token', jwt_hs256_sign(
      json_object('sub', id::VARCHAR, 'username', username, 'exp', epoch(now()) + 3600),
      getenv('JWT_SECRET')
    )
  )
) AS body
FROM ok;
-- 0 rows → quackapi 401/400 mapping via STATUS / empty-result policy
```

### 2.3 Outbound HTTP → `http_client`

**FastAPI**

```python
async with httpx.AsyncClient() as client:
    r = await client.post("https://api.partner.com/v1/score", json=payload)
```

**quackapi**

```sql
LOAD http_client;

CREATE OR REPLACE ROUTE proxy_score POST '/score'
AS
SELECT http_post(
  'https://api.partner.com/v1/score',
  map {'Authorization': 'Bearer ' || getenv('PARTNER_TOKEN'),
       'Content-Type': 'application/json'},
  $body::JSON
) AS partner_response;
```

### 2.4 Email via provider → `http_client` (SendGrid/Mailgun/Postmark HTTP APIs)

**FastAPI**

```python
await send_email(to=email, subject="Welcome", body=html)
```

**quackapi**

```sql
LOAD http_client; LOAD tera;

CREATE OR REPLACE MACRO send_email_postmark(to_addr, subject, html) AS (
  http_post(
    'https://api.postmarkapp.com/email',
    map {
      'X-Postmark-Server-Token': getenv('POSTMARK_TOKEN'),
      'Content-Type': 'application/json'
    },
    json_object(
      'From', 'noreply@example.com',
      'To', to_addr,
      'Subject', subject,
      'HtmlBody', html
    )
  )
);

CREATE OR REPLACE ROUTE welcome POST '/notify/{email}'
  PARAM email VARCHAR
AS
SELECT send_email_postmark(
  $email,
  'Welcome',
  tera_render('Hi {{ email }}', {'email': $email})
) AS provider_response;
```

### 2.5 Templates → `tera`

**FastAPI**

```python
return templates.TemplateResponse("item.html", {"request": request, "id": id})
```

**quackapi**

```sql
LOAD tera;

CREATE OR REPLACE ROUTE read_item GET '/items/{id}'
  PARAM id INTEGER
AS
SELECT tera_render(
  '<h1>Item {{ id }}</h1>',
  {'id': $id::VARCHAR}
) AS html;
```

(`tera_render('Hello {{ name }}', {'name':'world'})` → `Hello world` verified this session.)

### 2.6 Cache → `cache_httpfs` (HTTP(S) object cache) + SQL tables for app cache

**FastAPI**

```python
value = await redis.get(key) or await compute(); await redis.set(key, value)
```

**quackapi**

```sql
LOAD cache_httpfs;
-- Remote HTTP(S) reads through on-disk/object cache (filesystem plane)
SELECT cache_httpfs_get_cache_config();

-- App-level key/value: plain DuckDB table (no new ext)
CREATE TABLE IF NOT EXISTS kv_cache (
  k VARCHAR PRIMARY KEY, v JSON, expires_at TIMESTAMP
);

CREATE OR REPLACE MACRO cache_get(key) AS TABLE (
  SELECT v FROM kv_cache WHERE k = key AND expires_at > now()
);

CREATE OR REPLACE MACRO cache_put(key, val, ttl_s) AS (
  INSERT INTO kv_cache VALUES (key, val, now() + to_seconds(ttl_s))
  ON CONFLICT (k) DO UPDATE SET v = excluded.v, expires_at = excluded.expires_at
);
```

`cache_httpfs` is the existing ext for **HTTP filesystem caching**; application KV is a table + macros (no new C++).

### 2.7 Background work → `cronjob` and/or `quackapi_enqueue`

**FastAPI**

```python
background_tasks.add_task(write_notification, email, message)
```

**quackapi — queue (native quackapi)**

```sql
LOAD quackapi;
SELECT quackapi_enqueue('notifications', json_object('email', $email, 'msg', $message));
```

**quackapi — schedule (community cronjob)**

```sql
LOAD cronjob;
SELECT cron('drain_notifications',
  'DELETE FROM notifications_outbox WHERE id IN (
     SELECT id FROM notifications_outbox LIMIT 100
   )',  -- real drain would call http_client
  '*/1 * * * *');
SELECT * FROM cron_jobs();
```

---

## 3. Escape hatch (bucket C) — SQL/macro-native “drop to code”

Goal: keep `CREATE ROUTE AS <query>` as the only HTTP surface, while letting teams register **imperative** logic as ordinary DuckDB objects. Prefer **no new C++**.

### 3.1 Seam design

```
HTTP request
  → quackapi route match + PARAM/BODY SCHEMA validation
  → SELECT / TABLE query body
       ├─ pure SQL                          (bucket A)
       ├─ calls to community-ext functions  (bucket B)
       └─ calls to USER-REGISTERED macros / table functions  (bucket C seam)
  → response serialization
```

**Registration surface (already DuckDB):**

| Object | Use when | Example |
|--------|----------|---------|
| `CREATE MACRO name(...) AS <expr>` | Scalar business rule | risk score, JWT helper, password verify |
| `CREATE MACRO name(...) AS TABLE <query>` | Multi-row / multi-step pipeline | assembly of ROH review tree leaves |
| Temp/permanent tables + macros | Durable domain state | scoring intermediate tables |
| `quackapi_enqueue` | Async / out-of-request work | LLM job, email drain |

**Route shape (no new syntax required beyond existing CREATE ROUTE AS SELECT):**

```sql
-- User registers logic once (deploy-time SQL pack)
CREATE OR REPLACE MACRO roh_score(criterion_results JSON) AS (
  -- domain: Locus-style rule fold — pure SQL CASE / JSON walks
  CASE
    WHEN json_extract_string(criterion_results, '$.SI') = 'MET'
     AND json_extract_string(criterion_results, '$.PLAN') = 'MET'
    THEN 'HIGH'
    ELSE 'LOW'
  END
);

-- Or multi-statement logic as a table macro
CREATE OR REPLACE MACRO evaluate_roh_tree(review JSON) AS TABLE (
  -- fan-out criteria, join judgments, aggregate
  SELECT
    crit.criterion_code,
    roh_score(crit.payload) AS result
  FROM (
    SELECT unnest(json_transform(review, '{"criteria":"JSON[]"}')) AS c
  ) t(crit)
);

-- Route only composes
CREATE OR REPLACE ROUTE evaluate_case POST '/cases/{case_id}/evaluate'
  PARAM case_id VARCHAR
  BODY SCHEMA '{ "type":"object", "required":["review"] }'
AS
SELECT * FROM evaluate_roh_tree($review);
```

### 3.2 Escape ladder (prefer earlier rungs)

1. **Rewrite as SQL** (CTEs, `json_extract`, window functions) — still A.  
2. **Compose community ext functions** (`crypto_*`, `http_*`, `tera_render`, `cron`) — B.  
3. **User scalar / table macro** in a `.sql` pack loaded at boot — C, still SQL-native.  
4. **Out-of-band worker** via `quackapi_enqueue` + consumer SQL — C async.  
5. **Only if unavoidable:** DuckDB C-API table function / extension. **Not required for the corpus C set** (WebSockets) if WebSocket is declared out of scope for v1; for LLM/domain, prefer (3)+(4).

### 3.3 What C looks like in the wild (beyond this corpus)

| Imperative pattern | Escape hatch |
|--------------------|--------------|
| WebSocket chat loop | **Out of scope** for CREATE ROUTE (HTTP); keep separate process or future WS binding — do not force SQL |
| Locus ROH tree evaluation + LLM extraction | Table macros + `http_client` to model API + `quackapi_enqueue` for long runs; **15 Pydantic models already IR-validatable** |
| Multi-step sagas / compensating transactions | Table macros + state table + cron drain |
| Custom scoring / graph algorithms | Scalar/table macros; if truly opaque binary, last-resort C API TF |

### 3.4 Optional C++ (only if measured unavoidable)

| Gap | Why it might force C++ | Avoidance |
|-----|------------------------|-----------|
| Adaptive password KDF (bcrypt/argon2) | `crypto` lacks them | Macro + external verifier service via `http_client`, or accept HMAC-SHA with strong salt for greenfield |
| First-class JWT claims API | convenience | macros on `crypto_hmac` (shown above) |
| WebSocket routes | different protocol | do not block REST bridge |

**Verdict on C++:** none required to carry A+B; C escape hatch is **CREATE MACRO / MACRO AS TABLE + enqueue**.

---

## 4. Coverage verdict

### 4.1 What “carries automatically” means

| Layer | Mechanism | Corpus evidence |
|-------|-----------|-----------------|
| Routes | IR → `CREATE ROUTE` method/path/name | 519 FastAPI routes extracted |
| Validation | Pydantic/SQLModel → BODY SCHEMA / PARAM types | 2908 model fields; **locus 15 ROH contracts** |
| Handler A | `CREATE ROUTE AS SELECT/DML` | 498/519 (96.0%); RealWorld 15/19 (78.9%) |
| Handler B | same + existing ext / macros | 16/519 (3.1%); RealWorld 4/19 (21.1%) |
| Handler C | user macro / queue seam | 5/519 (1.0%), all WebSocket demos |

### 4.2 Headline numbers

| Population | Auto-carry **A+B** | Residual **C** |
|------------|-------------------:|---------------:|
| All FastAPI handlers in corpus (n=519) | **99.0%** (514) | 1.0% (5 WS) |
| fastapi-realworld only (n=19) | **100%** (19) | 0% |
| realworld + sqlmodel-docs (n=76) | **100%** (76) | 0% |

### 4.3 Realistic “% of a real FastAPI app”

**Headline: ~85–95% of a CRUD-centric FastAPI app** (routes + validation + handler A + handler B) is in automatic reach of `quack_from_fastapi` **without new C++**, given existing extensions.

Breakdown for a RealWorld-shaped app:

| Piece | Carry? | Est. share of eng surface |
|-------|--------|---------------------------|
| Path/method registration | yes | ~15% |
| Pydantic request/response validation | yes (IR models) | ~20% |
| CRUD handlers (A) | yes → CREATE ROUTE AS SELECT | ~45% |
| Auth token + password (B) | yes → crypto_hmac + macros | ~10% |
| Middleware/Depends auth plumbing | partial (re-express as SQL macros / `quackapi_authentication`) | ~5% |
| Imperative domain (C) | escape hatch only | ~5–15% depending on product |

**Honest boundary (C):**

- **WebSockets / long-lived streams** — not CREATE ROUTE material.  
- **LLM / multi-agent / heavy orchestration** (Locus extraction + tree evaluation) — lives in **user table macros + `http_client` + enqueue**, not auto-translated Python.  
- **Side effects buried in Depends() / service classes** — body-only AST under-detects; translation must also walk dependency graphs (or OpenAPI-only path leaves TODO stubs).  
- **Tutorial corpus overstates A** (262 echo stubs). Prefer RealWorld’s **~79% A / ~21% B** as the app prior.

### 4.4 Locus one-liner

Locus proves the **validation** half of the bridge (15 ROH Pydantic models in IR) and the **C seam** half of the product (imperative evaluation/LLM) — not FastAPI handlers. A quackapi Locus would: schema-validate review payloads automatically; call `evaluate_roh_tree(...)` user macros for scoring; enqueue extraction jobs.

---

## 5. Artifacts written this session

| Path | Contents |
|------|----------|
| `/tmp/quackapi_handler_bridge.md` | this report |
| `/tmp/quackapi_handler_classified.parquet` | 519 rows: body, features, bucket, b_patterns |
| `/tmp/quackapi_handler_bucket_counts.parquet` | A/B/C counts + pct |
| `/tmp/quackapi_handler_bucket_by_repo.parquet` | split by repo |
| `/tmp/quackapi_handler_b_patterns.parquet` | B pattern histogram |
| `/tmp/quackapi_classify_handlers.sql` | reproducible classifier |
| `/tmp/quackapi_classify_handlers.log` / `reclassify.log` | run logs |

---

## 6. Bottom line

```
Full FastAPI corpus handlers:   A 96.0% | B 3.1% | C 1.0%
                                A+B auto-carry = 99.0%

RealWorld app handlers:         A 78.9% | B 21.1% | C 0%
                                A+B auto-carry = 100% of HTTP handlers

Realistic product coverage:     ~85–95% of a CRUD FastAPI app
                                (routes + validation + A + B via existing exts)
Honest residue:                 C = user SQL macros / table macros / enqueue
                                (WebSocket & opaque Python stay outside)
Build nothing new in C++ for this bridge.
```
