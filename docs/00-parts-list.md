# Parts list — what a web framework is actually made of

> Goal of this doc: break "a web framework" into the **independent capabilities** it needs, so you can
> see exactly which we already have (native SQL), which come from a DuckDB extension (and which one), and
> which is an irreducible boundary. If you want to swap in a better community extension for any part, this
> is the map that tells you *which part* you're shopping for.

## The mental model (read this first if web frameworks are fuzzy)

A "web framework" like FastAPI feels like one magic thing, but it's really **two stacked layers** that
people conflate:

1. **The server (uvicorn).** A dumb, always-on program that owns a network port, accepts incoming
   connections, and reads the raw bytes of each HTTP request. It knows *nothing* about your app. It's
   plumbing. FastAPI does not do this — uvicorn does.
2. **The framework (FastAPI itself).** Pure logic that takes a parsed request and decides: which function
   handles `GET /users/123`? are the inputs valid? what JSON comes back? what does `/openapi.json` say?
   **This layer is just data transforms.** No sockets, no threads — just "given this request record,
   produce this response record."

Our entire thesis: **layer 2 is 100% expressible as DuckDB SQL.** Layer 1 (the socket) we borrow from an
extension. The only thing that's neither is the tiny translation between them.

Below, every capability is tagged:

- ✅ **NATIVE** — plain DuckDB SQL, no extension needed.
- 🧩 **EXTENSION** — provided by a DuckDB extension (named, and whether it loads here).
- 🚧 **BOUNDARY** — not a DuckDB thing at all; the uvicorn line.

---

## The parts

| # | Capability | What it actually means (plain) | Python/FastAPI world | Our stack | Status |
|---|-----------|--------------------------------|----------------------|-----------|--------|
| 1 | **Listener / socket** | Hold a network port open, accept incoming HTTP, run a query, return bytes | uvicorn | `quack` ext (`quack_serve`) — *loads* ✓. (`httpserver`, its successor, 404s on v1.5.3/arm64) | 🧩 have it |
| 2 | **Read the URL path off the wire** | Know that the browser asked for `/users/123` (not just `/`) | uvicorn parses the request line | the listener **throws the path away** (only sees `/`) → must be smuggled in | 🚧 **the boundary** |
| 3 | **Outbound HTTP from inside a query** | A running query can POST text to another HTTP endpoint | `requests` / `httpx` | `http_client` ext → `http_post_form(...)` — *loads* ✓ | 🧩 have it |
| 4 | **Run a query built at runtime** | Execute a SQL string you assembled on the fly (the handler) | Python just `exec`s your function | **self-dispatch**: POST the built SQL to #1 via #3 (a macro can't `EXECUTE` a string) | ✅+🧩 have it |
| 5 | **Route matching + capture** | Map `/users/{id}` → handler, pull out `id=123` | FastAPI decorators + Starlette router | pure SQL: split on `/`, segment-match, capture `{param}` slots — **PROVEN** | ✅ native |
| 6 | **Parse path + query string** | Turn `?q=x&limit=5` into typed fields | Starlette / Pydantic | `string_split` (native) + `parser_tools` + `sazgar` — *load* ✓ | ✅+🧩 have it |
| 7 | **Validation / coercion (Pydantic)** | "id must be a positive int" → 422 if not | Pydantic | `try_cast` + `CASE` + `json_type` guards + comparisons, aggregated into a 422 body | ✅ native |
| 8 | **Template rendering (handlers / HTML)** | Fill `{{ name }}` into a SQL or HTML template | Jinja2 | `tera` ext → `tera_render(tpl, json)` — *renders* ✓ | 🧩 have it |
| 9 | **JSON serialization (the response)** | Turn rows into a JSON body | Pydantic / `json` | `json_object`, `to_json`, `json_group_array` | ✅ native |
| 10 | **OpenAPI spec generation** | Auto-produce `/openapi.json` describing every route | FastAPI introspects type hints | a `SELECT` over the `routes` + `param_schema` tables (*easier* than FastAPI) | ✅ native |
| 11 | **Swagger UI page** | The interactive `/docs` web page | FastAPI ships it | `tera` renders one HTML page that loads Swagger from a CDN | 🧩 have it |
| 12 | **Concurrency / threads** | Handle many requests at once | uvicorn's event loop | the listener ext is multi-threaded; writes serialize (single-writer) | 🧩 have it (with a ceiling) |
| 13 | **Auth (Basic / API key)** | Reject unauthenticated callers | FastAPI dependencies / middleware | listener does Basic/`X-API-Key`; per-route checks are native SQL in the handler | ✅+🧩 have it |

---

## The verdict (the actual answer to "what don't we have natively?")

**Almost everything is native SQL.** The framework brain — routing (5), validation (7), serialization (9),
OpenAPI (10) — needs **zero extensions**.

**Three capabilities require an extension, and all three already load here:**

- **Listener (1)** → `quack` ✓
- **Outbound POST (3)** → `http_client` ✓
- **Templating (8)** → `tera` ✓

(`parser_tools` + `sazgar` are nice-to-haves for part 6, also loading ✓.)

**Exactly one thing is *not* a DuckDB problem at all — part 2, the boundary.** The listener discards the
URL path, so for a *browser* hitting `/users/123` something must copy that path into the SQL call. That's
~18 lines of stdlib Python (the "uvicorn-equivalent"), or a one-line proxy rewrite. It contains **zero
framework logic**. For non-browser clients you skip it entirely (they pass the path as an argument).

## The ONE extension worth hunting for 🔎

If you want to *erase the boundary* (part 2) and make it pure even for browsers, the extension to look for
is **a DuckDB HTTP-listener that exposes the full request — method, path, headers, body — to a SQL handler
(a "catch-all" / "predefined query" mode).** Today `quack`/`httpserver` only expose the body and ignore the
path. If such a listener exists (or a newer `httpserver` build adds a path-aware mode), part 2 flips from
🚧 boundary to ✅-via-extension, and the whole framework is 100% DuckDB end to end — no Python anywhere,
even for browsers. **That single capability — "request path → SQL" — is the only thing standing between
"pure framework + tiny shim" and "pure everything."**

Everything else: we already have it.
