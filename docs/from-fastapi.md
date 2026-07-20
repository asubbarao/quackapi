# Coming from FastAPI — `quack_from_X`

You already have routes, models, and maybe a monorepo of services. **`quack_from_X`** is the bridge family that **reads an existing app** and emits quackapi DDL (`CREATE ROUTE`, `BODY SCHEMA`, …) so you can serve a large fraction of the surface from DuckDB.

This page is honest about what translates automatically and what remains a human/escape-hatch step.

---

## Corpus (why this is real)

Across public frameworks, a multi-language IR corpus was extracted:

| Language | Routes (IR) | Model fields (IR) |
|----------|------------:|------------------:|
| Python | 990 | 2908 |
| Ruby | 52 | 22 |
| Go | 352 | 239 |
| Node | 182 | 1035 |
| **Total** | **1576** | **4204** |

- **36** top-level repos cloned under the corpus tree  
- Python tags include FastAPI (**519**), DRF viewsets (300), Flask (41), …  
- Source reports: corpus `PYTHON.md` / `RUBY.md` / `GO.md` / `NODE.md` (see also [FEATURE_STATUS §3](FEATURE_STATUS.md))

---

## What `quack_from_fastapi` does

**Status:** partial one-caller (shell + pure SQL). Not yet an in-tree C++ table function named `quack_from_fastapi(path)`.

Operational shape:

```text
/path/to/fastapi/app
        │
        ▼
quack_from_fastapi.sh  <app-path>  [port]  [serve|gen-only]
        │
        ├─ read Python AST (route decorators, include_router prefixes)
        ├─ walk Pydantic / SQLModel fields → JSON Schema
        ├─ emit CREATE OR REPLACE ROUTE … [BODY SCHEMA …] AS …
        ├─ FIFO session: LOAD quackapi; register routes; quackapi_serve
        ▼
live HTTP (path params + BODY SCHEMA validation)
```

Proven end-to-end (reports under the from-fastapi workdir):

| App | Result |
|-----|--------|
| fastapi-realworld | pass=10 fail=0 · 19 routes registered · 25 models |
| locus ROH sample | pass=10 fail=0 |

**Handler A/B/C split** over 519 FastAPI-tagged corpus routes:

| Bucket | Meaning | Share |
|--------|---------|------:|
| **A** | Pure CRUD / declarative → `CREATE ROUTE AS SELECT/DML` | 96.0% |
| **B** | Side effects covered by existing extensions (crypto/JWT/http client/…) | 3.1% |
| **C** | Imperative residue (e.g. WebSocket demos in that corpus) | 1.0% |
| **A+B auto-carry** | | **99.0%** |

Realistic product prior for a typical CRUD FastAPI app: roughly **85–95%** of routes+validation can land without new C++.

---

## How to point it at a repo

> The one-caller currently lives as a **companion script + SQL** outside the extension binary. Paths below match the proven driver layout; promote into-repo `examples/` / `bridges/` after community packaging if not already present.

```sh
# Conceptual invocation (driver shell)
quack_from_fastapi.sh /path/to/your/fastapi/project 8000 serve
```

What you need on the machine:

1. Built `duckdb -unsigned` with `LOAD quackapi`  
2. AST reader extension used by the bridge SQL (`sitting_duck` / `read_ast`)  
3. Python sources with recognizable `@app.get` / `APIRouter` / Pydantic models  

Modes:

| Mode | Behavior |
|------|----------|
| `gen-only` | Emit `routes.sql` only — review before serving |
| `serve` | Register + `quackapi_serve(port)` |

Inspect afterward:

```sql
SELECT name, method, pattern FROM quackapi_routes();
```

```sh
curl http://127.0.0.1:8000/docs
```

---

## Honest boundary

| Translates well | Escape hatch |
|-----------------|--------------|
| Path + methods + router prefixes | Imperative Python bodies with control flow, external SDKs, threads |
| Query/path types + Field constraints → `PARAM` / schema keywords | Custom dependency graphs that are not auth headers |
| Pydantic models → `BODY SCHEMA` (~81% feature map today) | ~19% needs binder polish (optional/null multi-error, strict format: email/uri/…) |
| Auth *shape* (API key / bearer) as `CREATE AUTH` sketches | Full OIDC login redirects, session cookies |
| CRUD SELECT/INSERT RETURNING façades | Arbitrary business logic — rewrite as SQL or call out to a function you load |

**Rule of thumb:**

- **Routing + validation** → automated  
- **Handler body** → SQL rewrite or keep a thin external worker and use [CREATE QUEUE](guide/queue.md) / HTTP clients from SQL  

WebSocket handlers will not translate to HTTP Upgrade routes (use [SSE](guide/stream.md)).

---

## Sibling bridges

| Bridge | Status |
|--------|--------|
| `quack_from_fastapi` | Partial one-caller; e2e green on realworld samples |
| `quack_from_rails` | Proven one-caller (partial runtime fidelity — SQL façades, not Ruby) |
| `quack_from_openapi` / JSON Schema | Designed + fixtures — highest-leverage multi-stack path |
| Express / Nest / Gin / DRF / … | IR extracted; dedicated one-callers not all shipped |

OpenAPI-universal path is often the best way in when you already export a spec:

```text
openapi.yaml  →  CREATE ROUTE + BODY SCHEMA  →  quackapi_serve
```

---

## Pydantic → BODY SCHEMA fidelity (today)

| Band | Coverage |
|------|----------|
| Real now (~81%) | scalars, required/default, lists, nested, Literal/enum via schema, Field min/max/length/pattern, inheritance flatten |
| Needs C++ (~19%) | field-level body `loc` parity, optional/null body defaults, multi-error aggregation, strict `format:` checkers |

Compose email validation with existing tabular/regex tools rather than reimplementing in the extension core.

---

## Next steps for a FastAPI team

1. Run the [five-line hello world](index.md) so `CREATE ROUTE` is muscle memory.  
2. Map your top 10 endpoints by hand using [routes](guide/routes-and-params.md) + [bodies](guide/request-bodies.md).  
3. Point `quack_from_fastapi` at a branch for bulk emit; review `routes.sql`.  
4. Replace imperative bodies with SQL or queue workers.  
5. Add [policies](guide/policies.md) where you previously had ad-hoc tenant filters in Depends.

Parity details: [fastapi-parity.md](fastapi-parity.md).
