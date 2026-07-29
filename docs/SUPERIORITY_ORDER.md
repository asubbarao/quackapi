# How we beat FastAPI (ordered)

**Not optional ceremony.** Work that violates this order is wrong work.

```
1. EQUIVALENCE  →  2. SURPASS (slaughter)  →  3. LEAPFROG
```

## 1. Equivalence

Same HTTP surface as FastAPI for what people actually ship:

| Area | Status |
|------|--------|
| Routes / methods / path / query / body / form / multipart | **done** (89/89 harness) |
| Validation 422 shape | **done** (multi-error polish still open ~19% Pydantic) |
| OpenAPI + /docs + /redoc | **done** |
| Auth API key + JWT bearer | **done** |
| CORS / OPTIONS / HEAD / 405 Allow | **done** |
| Redirect / cookies / headers | **done** |
| Static files / compression / request-id / health | **done** (static prefix polish open) |
| Background work | **QUEUE** (stronger than BackgroundTasks) |
| SSE streaming | **done** (WS = intentional SSE+radio, not Upgrade) |
| TestClient in-process | **done** — `quackapi_request(method, path [, body])` |
| Sessions / OIDC | **open** — after surpass holds |
| Response FORMAT (CSV/NDJSON/Parquet/Arrow) | **done** — FORMAT + Accept negotiation |

**Gate:** `test/conformance` 100% + `test/http/run_all.sh` green.  
Do not invent leapfrog nouns while equivalence cells are red.

## 2. Surpass (slaughter)

Same Postgres, same routes, **destroy** FastAPI on cost.

| Board | Rule |
|-------|------|
| `quackapi-bench` vs fastapi | **Every cell:** med latency ≤ FastAPI **and** prefer rps ≥ FastAPI |
| Under load (16–32 VUs) | Target **≥3×** med speedup and **≥2×** RPS (rows can be more) |
| Path | **`pg_dsn` libpq**, not ATTACH middleman, not local CTAS |

**Latest w1 board** (2026-07-24, 20s measure): see `quackapi-bench/bench/README.md`.  
Headline: item@32 **6.5×** faster / **5.6×** RPS; rows@32 **13×** / **8×**; write@32 **4.4×** / **4×**. **Zero lose cells.**

**Engineering that keeps the slaughter:**

- Body → libpq **before** DuckDB prepare  
- Flat JSON extract (no SQL body parse on POST)  
- PQprepare cache per worker thread  
- access_log / QueryLog off by default  
- Rebuild **shell + loadable** or you ship a lie  

If a cell loses: **fix the hot path** before adding features.

## 3. Leapfrog (only after 1–2)

Things FastAPI does not have as first-class product:

- Row access + masking **policies** as DDL  
- `CREATE QUEUE` durable jobs in the same DB  
- `CREATE API FOR TABLE`  
- `quack_from_openapi` / `quack_from_fastapi` migration gravity  
- SQL middleware / ETag table cache / Arrow export  

These are **wins**, but they do not excuse a lost item@32.

## Agent checklist

```
[ ] Equivalence tests green?
[ ] Bench: all cells win (re-run after hot-path change)?
[ ] Only then: leapfrog feature
[ ] Prefer less LOC / fix over new subsystem
```
