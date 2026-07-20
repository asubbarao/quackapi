# quack_from_rails — Rails → quackapi bridge

**Status:** proven on rails-realworld  
**Shape:** same as `quack_from_fastapi` (path → IR → CREATE ROUTE → FIFO register → serve)  
**Binary:** `/Users/aloksubbarao/personal/quackapi/build/release/duckdb -unsigned` + `LOAD quackapi`  
**No C++ changes** (read-only on extension; all work under `/tmp`)

---

## One-liner

**`quack_from_rails` is a real one-caller (partial runtime fidelity): one shell entrypoint imports Rails routes+validations into live quackapi; handlers are SQL façades, not Ruby controllers.** Next: `quack_from_express`.

---

## 1. Design

### Goal

First-class bridge: point at a Rails app root and get registered `CREATE ROUTE`s with:

- RESTful paths from `config/routes.rb` (`resources` / `resource` / `get|post|…` / `scope` / `namespace` / nested / collection)
- Path-param typing (`:id` → INTEGER → 422 on bad cast)
- Body validation from ActiveModel `validates` + strong params `require/permit` → JSON Schema `BODY SCHEMA` (Rails-wrapped: `{"article":{…}}`)

### Pipeline (mirrors fastapi)

```
/path/to/rails/app
        │
        ▼
 extract_rails_path.py          # sitting_duck language:='ruby'
   read_ast(routes.rb)          # + resources/scope expander (reuse corpus)
   read_ast(models/*.rb)        # validates → field IR
   read_ast(controllers/*.rb)   # strong_params → field IR
        │
        ▼
 ir_routes.parquet + ir_models.parquet   (Python-schema-aligned IR)
        │
        ▼
 quack_from_rails_core.sql      # pure SQL macros + tables
   merge validates ∩ permit
   BODY SCHEMA embed under resource key
   emit CREATE OR REPLACE ROUTE … (+ /_qf/validate/{Model})
        │
        ▼
 routes.sql.b64 → routes.sql
        │
        ▼
 FIFO interactive stdin         # one statement at a time
   LOAD quackapi;
   CREATE OR REPLACE ROUTE …;   # × N
   SELECT * FROM quackapi_routes();
   SELECT * FROM quackapi_serve(port);
        │
        ▼
 curl ≥3 endpoints (200/201/422)
```

### Why hybrid extract + SQL emit

| Layer | Why |
|-------|-----|
| **Python expander** (`extract_ruby_ir` reused) | Rails `resources` expansion + nested blocks + `only`/`except`/`param` is block-structured; sitting_duck `peek` includes whole `do…end` — header-line isolation is proven in corpus |
| **SQL core** | Same CREATE ROUTE / BODY SCHEMA / path-param emission as fastapi; no new C++ |
| **FIFO shell** | quackapi `parser_extension` one-statement-at-a-time gotcha — never `duckdb -c` for DDL+serve |

### IR schema (parity with Python)

**Routes:** `framework, method, path, handler_name, file, start_line, repo, evidence`  
**Models:** `framework, model_name, field_name, field_type, is_optional, has_default, is_required, default_expr, file, field_line, repo, declared_annotation`

### Mapping table

| Rails | quackapi |
|-------|----------|
| `resources :articles` index/show/… | `CREATE ROUTE get_api_articles GET '/api/articles' AS SELECT …` |
| `articles#create` + `validates :title, presence: true` | `POST … BODY SCHEMA {"article":{"required":["title","body"],…}} STATUS 201` |
| `params.require(:article).permit(:title,:body,…)` | Nested wrap key `article`; permit fields become properties (not auto-required) |
| Path `:id` / `:slug` | `PARAM id INTEGER` / `PARAM slug VARCHAR` + `$id` cast → **422** on type error |
| Controller action body | **SQL SELECT façade** (handler name echo) — not Ruby runtime |
| Devise / callbacks / AR | Out of scope (static IR only; devise_for → login/logout stubs) |

### SQL-handler boundary

quackapi handlers are pure SQL. Rails controller/filter/ORM code is **source for IR extraction**, not the runtime. The bridge is **parse → IR → emit CREATE ROUTE**, not embed a Ruby VM.

---

## 2. Code locations

| Path | Role |
|------|------|
| `/tmp/quackapi_fromrails/quack_from_rails.sh` | **One-caller** driver: extract → gen → FIFO serve |
| `/tmp/quackapi_fromrails/quack_from_rails_core.sql` | IR → CREATE ROUTE + BODY SCHEMA + summary |
| `/tmp/quackapi_fromrails/extract_rails_path.py` | Path-scoped extract (reuses corpus expander) |
| `/tmp/quackapi_corpus/ruby/extract_ruby_ir.py` | Canonical resources/scope/validates expander |
| `/tmp/quackapi_fromrails/prove_e2e.sh` | E2E: serve + curl matrix |
| `/tmp/quackapi_fromrails/out/rails-realworld/` | Generated IR, routes.sql, transcript, summary |
| `/tmp/quackapi_corpus/ir_ruby_routes.parquet` | Corpus IR (52 routes / multi-repo) |
| `/tmp/quackapi_corpus/ir_ruby_models.parquet` | Corpus model IR (22 fields) |
| `/tmp/quackapi_corpus/RUBY.md` | Corpus extraction notes |

### Usage

```bash
# one call
/tmp/quackapi_fromrails/quack_from_rails.sh /tmp/quackapi_corpus/ruby/rails-realworld 18926 serve

# gen only
/tmp/quackapi_fromrails/quack_from_rails.sh /path/to/app 18926 gen-only

# prove
/tmp/quackapi_fromrails/prove_e2e.sh /tmp/quackapi_corpus/ruby/rails-realworld 18926
```

### Core SQL emit (excerpt)

```sql
CREATE OR REPLACE ROUTE post_api_articles POST '/api/articles' STATUS 201
  BODY SCHEMA '{"type":"object","required":["article"],"properties":{"article":{"type":"object","required":["body","title"],"properties":{"body":{"type":"string","minLength":1},"description":{"type":"string"},"slug":{"type":"string"},"title":{"type":"string","minLength":1}}}}}'
  AS
SELECT 'articles#create' AS handler, 'article' AS body_model, 'ok' AS status;

CREATE OR REPLACE ROUTE delete_api_articles_slug_comments_id
  DELETE '/api/articles/:slug/comments/:id'
  PARAM id INTEGER
  PARAM slug VARCHAR
  AS
SELECT $id::INTEGER AS id, $slug::VARCHAR AS slug, 'comments#destroy' AS handler, 'DELETE' AS method;
```

---

## 3. Proof transcript (rails-realworld)

**App:** `/tmp/quackapi_corpus/ruby/rails-realworld` (gothinkster RealWorld)  
**Port:** `18926`  
**Full log:** `/tmp/quackapi_fromrails/out/rails-realworld/e2e_transcript.txt`

### Registry (`quackapi_routes` via CREATE status lines)

All 21 imported routes registered, plus health + 3 model façades (25 CREATE ROUTE stmts):

```
Route qf_health: GET /_qf/health
Route get_api_articles: GET /api/articles
Route post_api_articles: POST /api/articles
Route get_api_articles_slug: GET /api/articles/:slug
Route patch_api_articles_slug: PATCH /api/articles/:slug
Route put_api_articles_slug: PUT /api/articles/:slug
Route delete_api_articles_slug: DELETE /api/articles/:slug
Route get_api_articles_slug_comments: GET /api/articles/:slug/comments
Route post_api_articles_slug_comments: POST /api/articles/:slug/comments
Route delete_api_articles_slug_comments_id: DELETE /api/articles/:slug/comments/:id
Route post_api_articles_slug_favorite: POST /api/articles/:slug/favorite
Route delete_api_articles_slug_favorite: DELETE /api/articles/:slug/favorite
Route get_api_articles_feed: GET /api/articles/feed
Route get_api_profiles_username: GET /api/profiles/:username
Route post_api_profiles_username_follow: POST /api/profiles/:username/follow
Route delete_api_profiles_username_follow: DELETE /api/profiles/:username/follow
Route get_api_tags: GET /api/tags
Route get_api_user: GET /api/user
Route patch_api_user: PATCH /api/user
Route put_api_user: PUT /api/user
Route post_api_users_login: POST /api/users/login
Route delete_api_users_logout: DELETE /api/users/logout
Route validate_Comment / validate_article / validate_user
```

### Curl results (14/14 PASS)

```
PASS  health                                              → 200
PASS  GET /api/tags                                       → 200
PASS  GET /api/articles                                   → 200
PASS  GET /api/articles/how-to-train-your-dragon          → 200  ($slug)
PASS  GET /api/profiles/jake                              → 200  ($username)
PASS  DELETE /api/articles/x/comments/abc                 → 422  (id type_error)
PASS  DELETE /api/articles/x/comments/7                   → 200  ($id INTEGER)
PASS  POST /api/articles valid body                       → 201  (BODY SCHEMA)
PASS  POST /api/articles missing title                    → 422  (required title)
PASS  POST /api/articles empty title                      → 422  (minLength:1)
PASS  POST /_qf/validate/article valid                    → 200
PASS  POST /_qf/validate/article missing body             → 422
PASS  GET /api/articles/feed                              → 200
PASS  DELETE /api/users/logout                            → 200
RESULT: pass=14 fail=0
```

### Sample validation bodies

```
# 422 path type
{"detail":[{"loc":["path","id"],"msg":"Input should be a valid integer","type":"type_error"}]}

# 422 body required
… required property 'title' not found in object …

# 422 minLength
… instance is too short as per minLength:1 …
```

---

## 4. Coverage numbers

| Metric | Value |
|--------|------:|
| **Routes found** (IR from routes.rb) | **21** |
| **Routes resolved** | **21** |
| **Routes registered** (CREATE ROUTE kind=route) | **21** |
| Model validate façades | 3 |
| Health | 1 |
| **Total CREATE ROUTE stmts** | **25** |
| Routes with BODY SCHEMA | 6 |
| Routes with path params | 12 |
| Models merged (article/comment/user) | 3 |
| Model fields | 10 |
| routes.rb AST nodes (sitting_duck) | 228 |
| models AST nodes | 558 |
| **curl working** | **14 / 14** |
| curl fail | 0 |

### Working definition

| Bucket | Count | Notes |
|--------|------:|-------|
| found | 21 | Expanded IR from RealWorld routes.rb |
| registered | 21 (+4 aux) | Live in `quackapi_routes()` after FIFO |
| working (curl) | 14 | All exercised endpoints behave (200/201/422) |

### Extraction coverage (DSL)

| Surface | Level |
|---------|-------|
| `get/post/put/patch/delete` + `to:` | high |
| `resources` / `resource` + only/except/param | high |
| Nested resources + collection (`get :feed`) | high |
| `scope` / `namespace` prefix | high |
| ActiveModel `validates` presence → required + minLength | high |
| Strong params permit → properties (not required) | high |
| `devise_for` | low (login/logout stubs only) |
| Concerns / engines / runtime constraints | none |

---

## 5. Next first-class `quack_from_X` (popularity-ranked)

Ranked by real-world web/API framework popularity (GitHub stars + Stack Overflow + job-market signal, 2025–2026). One line each:

| Rank | Bridge | Why next |
|------|--------|----------|
| 1 | **`quack_from_express`** | Node’s default HTTP API surface; largest non-Python/non-Rails install base; IR already in corpus (`ir_node_routes`) |
| 2 | **`quack_from_flask`** | Second Python microframework after FastAPI in many apps; decorator routes already in `extract_python_ir.sql` |
| 3 | **`quack_from_django` / DRF** | Still #1 Python full-stack; viewsets + serializers map cleanly to BODY SCHEMA (IR partially extracted) |
| 4 | **`quack_from_gin`** | Go’s dominant HTTP router; struct tags → validation IR already proven in GO.md corpus |
| 5 | **`quack_from_nestjs`** | Enterprise Node; decorator metadata ≈ FastAPI shape once express IR lands |
| 6 | **`quack_from_laravel`** | PHP’s top framework; routes/api.php + FormRequest validation ≈ Rails strong params |
| — | Sinatra | Already secondary in Ruby IR (8 routes); too small for first-class next |

**Priority recommendation:** Express next (closes Node), then Flask/DRF (completes Python triad), then Gin (completes Go).

---

## 6. Improvements made this run

1. **Path-scoped extractor** — any Rails app root, not only corpus batch  
2. **Strong-params noise filter** — drop `before_action` / devise sanitizer false positives (`Params` junk)  
3. **Required semantics** — only ActiveModel presence marks required; permit is whitelist only  
4. **Rails body wrap** — `require(:article)` → nested JSON Schema under resource key  
5. **FIFO one-caller** — same serve path as fastapi; disown hold so piped prove scripts exit  

---

## 7. Verdict

| Question | Answer |
|----------|--------|
| Is `quack_from_rails('/path')` a real one-caller? | **Yes (partial)** — one script: extract → register → serve |
| Full Rails runtime? | **No** — SQL façades + validation, not AR/Devise/filters |
| Proof bar met? | **Yes** — `quackapi_routes()` shows imports; ≥3 curls work; 14/14 e2e |
| Next bridge | **`quack_from_express`** |

**`quack_from_rails` = real one-caller yes/partial + next=`quack_from_express`.**
