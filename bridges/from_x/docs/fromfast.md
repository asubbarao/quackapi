# quack_from_fastapi — flagship one-caller (Task #26)

**Date:** 2026-07-20  
**Binary (read-only):** `/Users/aloksubbarao/personal/quackapi/build/release/duckdb -unsigned`  
**Extensions used (existing only):** `quackapi`, `sitting_duck`, `parser_tools`, `json`  
**No C++ / no src edits.** All artifacts under `/tmp/quackapi_fromfast/`.

**Verdict (one line):** **partial** — `quack_from_fastapi('/path')` is a real one-call *driver* (shell + pure SQL) that clones/reads AST → IR → CREATE ROUTE + BODY SCHEMA → FIFO register → serve; next step is C++ `quack_from_fastapi(path)` table function so the name is a true SQL scalar/TVF inside one DuckDB session without a shell wrapper.

---

## 1. One-call design

```
/path/to/app
    │
    ▼
quack_from_fastapi.sh  (ONE call)
    │  1. pick glob: app/**/*.py | src/**/*.py | **/*.py
    │  2. sed-substitute {{APP_GLOB}}/{{REPO}} into core SQL
    │  3. duckdb -unsigned < core.sql   # AST→IR→DDL generation only
    │  4. decode base64 CREATE ROUTE statements → routes.sql
    │  5. FIFO interactive session (NOT duckdb -c):
    │        LOAD quackapi;
    │        CREATE OR REPLACE ROUTE …;   # one stmt at a time
    │        SELECT * FROM quackapi_routes();
    │        SELECT * FROM quackapi_serve(port);
    ▼
live HTTP  (path params + BODY SCHEMA validation)
```

### Why shell + SQL (not a pure MACRO alone)

- `sitting_duck.read_ast` requires **literal** globs (no column-parameterized path).
- quackapi `CREATE ROUTE` is a **parser extension** — must be fed via interactive/FIFO stdin, never `duckdb -c "a;b"`.
- Therefore the one-caller is `quack_from_fastapi.sh /path [port] [serve|gen-only]`, which is the operational equivalent of `quack_from_fastapi('/path')`.

### Code locations

| Artifact | Role |
|----------|------|
| `/tmp/quackapi_fromfast/quack_from_fastapi.sh` | **ONE call** entrypoint |
| `/tmp/quackapi_fromfast/quack_from_fastapi_core.sql` | Pure SQL: AST → IR → CREATE ROUTE + BODY SCHEMA |
| `/tmp/quackapi_fromfast/prove_e2e.sh` | E2E curl harness |
| `/tmp/quackapi_fromfast/out/<repo>/` | routes.sql, summary.json, transcripts, pids |

### Pipeline stages (core SQL)

1. **AST ingest** — `read_ast('{{APP_GLOB}}', 'python', peek:='full')` via sitting_duck  
2. **FastAPI routes** — same decorator join as corpus `extract_python_ir.sql` (`decorated_definition` → `call.name` ∈ get/post/…)  
3. **`include_router` prefixes** — extract `prefix="/users"` etc.; join with `api_prefix` default `/api`  
4. **Pydantic models** — transitive `BaseModel`/`SQLModel`/`RWModel`/`RWSchema` closure + **inheritance-flattened fields**  
5. **Body bind** — handler params of type `typed_default_parameter` with `Body(..., embed=True, alias=…)` → nested JSON Schema  
6. **Path params** — `{slug}` → `PARAM slug VARCHAR|INTEGER` (name heuristic: `*_id` → INTEGER)  
7. **Emit** — `CREATE OR REPLACE ROUTE` echo stubs + `POST /_qf/validate/{Model}` façades for every model  
8. **Register** — FIFO feed + `quackapi_serve`

### Existing extensions mapped (anti-bloat)

| Need | Extension / feature |
|------|---------------------|
| Python AST | `sitting_duck` `read_ast` |
| Route registry + HTTP | `quackapi` `CREATE ROUTE`, `quackapi_routes()`, `quackapi_serve()` |
| Body validation | `CREATE ROUTE … BODY SCHEMA` (community `json_schema` under the hood) |
| Path/query type 422 | `PARAM name TYPE` + `$name::TYPE` casts |
| IR pattern | corpus `extract_python_ir.sql` + openapi_gen BODY SCHEMA emission |

---

## 2. End-to-end transcripts

### 2a. fastapi-realworld — **pass=10 fail=0**

```text
$ /tmp/quackapi_fromfast/quack_from_fastapi.sh \
    /tmp/quackapi_corpus/python/fastapi-realworld 18826 serve

# summary (command output)
routes_found=19  routes_registered_sql=19  models_found=25
model_validate_routes=25  total_create_route_stmts=45
routes_with_body_schema=6  routes_with_path_params=11
include_router_calls=9  ast_nodes=18176  ast_files=72

# quackapi_routes() after FIFO register
n_routes = 45

# live curls (all PASS)
PASS  health  status=200  body=[{"status":"ok","repo":"fastapi-realworld"}]
PASS  GET /api/tags  status=200  body=[{"handler":"get_all_tags",...}]
PASS  GET /api/articles/how-to-train-your-dragon  status=200  body=[{"slug":"how-to-train-your-dragon"}]
PASS  DELETE comment_id=abc → 422  status=422
      body={"detail":[{"loc":["path","comment_id"],"msg":"Input should be a valid integer","type":"type_error"}]}
PASS  DELETE comment_id=7 → 200  status=200  body=[{"comment_id":7,"slug":"x"}]
PASS  POST login valid → 201  status=201
      body=[{"handler":"login","body_model":"UserInLogin","status":"ok"}]
PASS  POST login missing password → 422  status=422
      body=... required property 'password' not found ... (BODY SCHEMA on embed user)
PASS  POST /_qf/validate/UserInLogin valid → 200
PASS  POST /_qf/validate/UserInLogin missing → 422
PASS  GET /api/profiles/jake  status=200  body=[{"username":"jake"}]
```

Full transcript: `/tmp/quackapi_fromfast/out/fastapi-realworld/e2e_transcript.txt`  
Resolved paths include real mounts: `/api/users/login`, `/api/articles/{slug}`, `/api/tags`, …

### 2b. locus-review-interview — **pass=10 fail=0**

Locus has **no FastAPI routes** (Django admin only + ROH Pydantic contracts). The one-caller still wins: 16 model validation façades from AST.

```text
$ /tmp/quackapi_fromfast/quack_from_fastapi.sh \
    /tmp/quackapi_corpus/python/locus-review-interview 18827 serve

# summary
routes_found=0  models_found=17  model_fields=74
model_validate_routes=16  total_create_route_stmts=17
ast_nodes=20315  ast_files=76

# quackapi_routes()
n_routes = 17   # health + 16 validate_* (skips _Judgment private)

# live curls (all PASS)
PASS  health  status=200
PASS  POST /_qf/validate/SuicidalIdeation valid → 200  {"present":true,...}
PASS  POST SuicidalIdeation missing present → 422
PASS  POST /_qf/validate/HomicidalIdeation valid → 200
PASS  POST HomicidalIdeation present="yes" → 422  (type_error via schema)
PASS  POST SelfCareJudgmentOutput {} → 200
PASS  POST PastAttempt {} → 200
PASS  POST malformed JSON → 422  json_invalid
PASS  POST ExtractionEvidence valid → 200
PASS  POST ExtractionEvidence missing evidence_text → 422
```

Full transcript: `/tmp/quackapi_fromfast/out/locus-review-interview/e2e_transcript.txt`  
ROH models proven: `SuicidalIdeation`, `HomicidalIdeation`, `SelfCareJudgmentOutput`, `PastAttempt`, `ExtractionEvidence`, `SuicidalityOutput`, …

---

## 3. Coverage numbers per repo

| Metric | fastapi-realworld | locus-review-interview |
|--------|------------------:|-----------------------:|
| AST files / nodes | 72 / 18 176 | 76 / 20 315 |
| **Routes found** (FastAPI decorators) | **19** | **0** |
| Routes resolved (prefix-joined) | 19 | 0 |
| **Routes registered** (`quackapi_routes`) | **45** (= 19 + 25 model validate + 1 health) | **17** (= 16 model validate + 1 health) |
| Models found / fields | 25 / 86 | 17 / 74 |
| Routes with BODY SCHEMA (FastAPI handlers) | 6 | 0 (N/A) |
| Routes with path PARAM typing | 11 | 0 |
| **Curl probes fully-working** | **10 / 10** | **10 / 10** |
| Sample working endpoints | tags, articles/{slug}, comments/{id} 200/422, login 201/422, validate UserInLogin | ROH validate 200/422, JSON invalid 422 |

### Gap analysis

| Gap | fastapi-realworld | locus | Why |
|-----|-------------------|-------|-----|
| Handler business logic | All handlers are SQL echo stubs | N/A | quackapi handlers are SELECT — cannot transpile Python/JWT/DB |
| Nested `include_router` parent chain | Best-effort (module→prefix); works for this app’s flat mounts | N/A | No recursive mount graph; `prefix=settings.api_prefix` resolved via field default `"/api"` |
| Optional query defaults | Not emitted | N/A | IR has defaults; quackapi optional PARAM surface is partial |
| `Literal[...]` enum allow-list | N/A | Mapped as JSON `string` only (not enum) | Need enum CHECK / allowed-values in C++ or schema `enum` |
| Nested model recursion in BODY SCHEMA | Flat `type:object` for nested names | Same | One-level schema; no `$ref` graph |
| Private models (`_Judgment`) | Skipped | Skipped | Intentional filter `NOT starts_with(name,'_')` |
| Django/Flask routes | Not in this one-caller (FastAPI+Pydantic focus) | Django admin only — ignored | Separate corpus extractors exist |
| True SQL `SELECT quack_from_fastapi(path)` | Shell wrapper | Shell wrapper | Needs C++ TVF (see punch list) |

**Import coverage summary**

| Repo | found → registered → curl-working | Notes |
|------|-------------------------------------|-------|
| fastapi-realworld | 19 → 19 (+25 model façades) → 10/10 probes | All decorator routes registered; business logic not auto-ported |
| locus-review-interview | 0 FastAPI routes → 16 model routes → 10/10 probes | Validation-first offboard of ROH contracts |

---

## 4. C++ punch list (do **not** edit C++ in this task)

| Priority | Item | Why |
|----------|------|-----|
| **P0** | `quack_from_fastapi(VARCHAR path)` table function / pragma | True one-SQL-call without shell; inject path into sitting_duck + FIFO-free CREATE ROUTE batch API |
| **P0** | Batch `CREATE ROUTE` / `RegisterRoutes(list)` API usable from a single statement | Removes parser_extension one-statement FIFO gotcha for automation |
| **P1** | Optional PARAM + defaults (`DEFAULT NULL` already partial; default literals) | Pydantic optionals dominate |
| **P1** | BODY SCHEMA nested `$ref` + deeper property binding into `$params` | Embed + nested models |
| **P1** | `enum` / Literal allow-list on BODY SCHEMA or PARAM | ROH `Literal["current","recent","past"]` |
| **P2** | Recursive `include_router` mount graph + dynamic prefix resolution | Multi-level apps beyond string defaults |
| **P2** | Response model IR column + OpenAPI export of generated routes | Close the loop with openapi_gen |
| **P3** | `Depends()` graph awareness (skip as body) | Already filtered via `Body(` only; C++ could help signature analysis |

---

## 5. How to re-run

```bash
# generate only
/tmp/quackapi_fromfast/quack_from_fastapi.sh /tmp/quackapi_corpus/python/fastapi-realworld 18826 gen-only

# generate + register + serve
/tmp/quackapi_fromfast/quack_from_fastapi.sh /tmp/quackapi_corpus/python/fastapi-realworld 18826 serve
/tmp/quackapi_fromfast/quack_from_fastapi.sh /tmp/quackapi_corpus/python/locus-review-interview 18827 serve

# prove
/tmp/quackapi_fromfast/prove_e2e.sh /tmp/quackapi_corpus/python/fastapi-realworld 18826
```

---

## 6. Final line

**is `quack_from_fastapi('/path')` a real one-caller now?** → **partial**  
(shell+SQL one-call works end-to-end with validation on both repos; not yet a single in-process SQL function)

**Next step:** implement C++ `quack_from_fastapi(path)` (or `CALL quack_from_fastapi(...)`) that owns path→AST→register without FIFO/shell, reusing this SQL IR logic as the reference semantics.
