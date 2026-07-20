# `quack_from_x` — framework-import bridges

This is the permanent home for the `sitting_duck`-powered "point quackapi at an
existing web-framework repo and get live routes + validation" work. It
previously existed only as proven `/tmp` artifacts (never committed anywhere)
— this directory, `test/sql/quackapi_from_x_bridge.test`, and the branch
`feat/from-x` are that work's first real home.

**One-line status:** the *extraction* half (AST → routes + validation IR) is
proven across five language corpora (Python, Node/TS, Go, Ruby, plus a
committed test fixture); the *one-caller* half (`quack_from_fastapi('/path')`
as a single SQL call) is a real shell+SQL driver today, not yet a native
`quack_from_fastapi(path)` table function — see §4.

## What's in here

```
bridges/from_x/
├── README.md                    this file
├── extract/                      sitting_duck AST → IR extractors, one per language
│   ├── extract_python_ir.sql     FastAPI / Flask / Django+DRF / SQLModel / marshmallow / Pydantic
│   ├── extract_node_ir.sql       Express / NestJS / Fastify / Koa / zod / class-validator
│   ├── extract_go_ir.sql         Echo / Gin / Chi / Fiber
│   └── extract_ruby_ir.py        Rails / Sinatra (routes.rb expander + ActiveModel validates)
├── one_callers/                  the "point at a path, get a live server" drivers
│   ├── quack_from_fastapi.sh     ONE call: path → AST → IR → CREATE ROUTE → FIFO register → serve
│   └── quack_from_fastapi_core.sql   pure SQL body of the driver (AST→IR→DDL, no shell logic)
├── fixtures/
│   └── fastapi_mini/app/main.py  tiny committed FastAPI app used by the sqllogictest
└── docs/                         the original proof reports, unedited
    ├── fromfast.md               quack_from_fastapi e2e proof (fastapi-realworld, locus-review-interview)
    ├── rails_bridge.md           quack_from_rails e2e proof (rails-realworld)
    ├── pydantic_bridge.md        Pydantic feature-frequency → validation coverage table
    └── handler_bridge.md         FastAPI handler-body A/B/C classification (what auto-carries vs. doesn't)

test/sql/quackapi_from_x_bridge.test   sqllogictest: runs the route+model extraction
                                        against fixtures/fastapi_mini and asserts the
                                        expected routes/models come out.
```

## What translates automatically

Two things are fully declarative and map onto existing quackapi + community
extensions with **no new C++**:

1. **Routing.** Framework decorator/DSL syntax (`@router.get("/x")`, Express
   `app.get(...)`, Rails `resources :articles`, Echo `e.GET(...)`) is parsed by
   `sitting_duck.read_ast` into a `(method, path, handler_name)` IR and emitted
   as `CREATE OR REPLACE ROUTE <handler> <METHOD> '<path>' AS SELECT ...`. Path
   params (`{slug}`, `:id`) become `PARAM name TYPE` with automatic 422s on bad
   casts. `include_router(prefix=...)` / Rails `scope`/`namespace` nesting is
   resolved and joined into the final mount path.
2. **Request-body validation.** Pydantic/`class-validator`/Rails
   `validates`+strong-params fields are resolved (including one level of
   inheritance flattening) into a JSON Schema and attached as
   `CREATE ROUTE ... BODY SCHEMA {...}`, which the community `json_schema`
   machinery enforces — required/optional, type mismatches, and missing
   properties all come back as real 422s.

Measured on the FastAPI corpus (`docs/handler_bridge.md`, `docs/pydantic_bridge.md`):
**96% of 519 real-world FastAPI handler bodies are pure declarative CRUD**
(bucket A) that a plain `CREATE ROUTE ... AS SELECT` can carry, and **~81% of
observed Pydantic field features** (scalar types, required/optional,
defaults, one level of nesting) map onto `BODY SCHEMA` validation as-is.

## The honest escape hatch — what does NOT auto-translate

**Imperative handler bodies are never transpiled.** quackapi routes are
`SELECT`/`TABLE` queries; a handler that does non-declarative work (JWT
minting, bcrypt/argon2 hashing, a WebSocket loop, a multi-step saga, a call
into an LLM, hand-rolled business logic) has no Python/Ruby/Go/TS→SQL
transpiler and never will. The bridge's answer to that ~1-4% is: register the
logic as an ordinary DuckDB object and call it from the route body —

| Need | Escape hatch |
|------|---------------|
| Scalar business rule (risk score, password check) | `CREATE MACRO name(...) AS <expr>`, called from `CREATE ROUTE ... AS SELECT my_macro($x)` |
| Multi-row / multi-step pipeline | `CREATE MACRO name(...) AS TABLE <query>` |
| Durable domain state | permanent tables + macros |
| Async / out-of-request work | `quackapi_enqueue` |
| Truly opaque binary logic (bcrypt/argon2 KDF, WebSocket protocol) | out of scope for `CREATE ROUTE` (HTTP-only); keep as a separate process, or the last-resort C API table function |

See `docs/handler_bridge.md` §3 ("Escape hatch (bucket C)") for the full
seam design: `HTTP request → quackapi PARAM/BODY SCHEMA validation → SELECT
query body → { pure SQL | community-ext calls | user-registered macros } →
response`. No new CREATE ROUTE syntax is required for any of it — the seam is
just "call a macro from the SELECT."

## `quack_from_fastapi` — the one-caller, today

```
/path/to/app
    │
    ▼  bridges/from_x/one_callers/quack_from_fastapi.sh /path [port] [serve|gen-only]
    │    1. pick glob (app/**/*.py | src/**/*.py | **/*.py)
    │    2. duckdb -unsigned < quack_from_fastapi_core.sql   # AST → IR → CREATE ROUTE DDL only
    │    3. FIFO interactive session (never `duckdb -c`, quackapi's
    │       CREATE ROUTE is a parser extension fed one statement at a time):
    │         LOAD quackapi; CREATE OR REPLACE ROUTE ...; SELECT * FROM quackapi_serve(port);
    ▼
live HTTP with path-param + BODY SCHEMA validation
```

Proven end-to-end on `fastapi-realworld` (19 routes → 45 registered routes
incl. 25 model-validation façades, 10/10 live curl probes 200/201/422) and
`locus-review-interview` (0 FastAPI routes, but 16 Pydantic model-validation
routes registered and proven — see `docs/fromfast.md` §2). `quack_from_rails`
mirrors the same shape for `config/routes.rb` + ActiveModel validations
(`docs/rails_bridge.md`).

**Why this is "partial" and not "done":** `quack_from_fastapi('/path')` today
is a shell entrypoint, not a single in-process SQL call, because
(a) `sitting_duck.read_ast` requires a *literal* glob argument — no
column-parameterized/lateral path — and (b) quackapi's `CREATE ROUTE` parser
extension must be fed one statement at a time over an interactive/FIFO
session, never `duckdb -c "a;b"`. Making `quack_from_fastapi(path)` a true SQL
table function needs a small C++ surface (see the punch list in
`docs/fromfast.md` §4 — P0 items are a `quack_from_fastapi(path)` TVF and a
batch `CREATE ROUTE` API that removes the one-statement-at-a-time
constraint). That C++ work is explicitly out of scope here; this commit homes
the SQL/shell driver that is the reference semantics for it.

## Running the test

```bash
cd bridges/from_x   # or from repo root — path in the .test file is repo-relative
# via the repo's own build:
./build/release/test/unittest test/sql/quackapi_from_x_bridge.test
```

The test loads `sitting_duck`/`parser_tools` (cached community extensions —
no network required once installed once), reads the AST of
`fixtures/fastapi_mini/app/main.py`, and asserts:

- exactly 2 routes are found (`GET /articles/{slug}`, `POST /login`), matching
  the decorator-join logic in `extract/extract_python_ir.sql` §2a
- exactly 1 Pydantic model (`UserInLogin`) with 3 fields is found, matching
  the `BaseModel` inheritance-closure logic used for `docs/pydantic_bridge.md`

## Re-running the corpus extractors

`extract/*.sql` and `extract_ruby_ir.py` are the originals used to build the
~1576-route / ~4204-model IR referenced throughout `docs/*.md` (36 repos
across Python/Node/Go/Ruby). They expect a `/tmp/quackapi_corpus/<lang>/...`
checkout layout and are **not** re-run in CI — they're kept here as the
reference implementation the fixture test is a minimal, committed proof of.
