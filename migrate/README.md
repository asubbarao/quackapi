# migrate/ — FastAPI → quackapi offboarding tool

Reads a FastAPI codebase with `sitting_duck` (AST-based Python parser), extracts
routes and models, and emits quackapi registration SQL. Comes with a safety-first
coverage report so nothing silently vanishes.

## Prerequisites

- `/opt/homebrew/bin/duckdb` built with `-unsigned` (community extension support)
- `sitting_duck` (auto-installed from community on first run)

## Quick start

```bash
# Single file
bash migrate/run_migrate.sh path/to/app.py

# Whole repo (glob must be quoted to prevent shell expansion)
bash migrate/run_migrate.sh 'path/to/repo/**/*.py'

# Write registration SQL to a file
bash migrate/run_migrate.sh 'path/to/repo/**/*.py' > generated_routes.sql
```

Or run manually in DuckDB (the source path is an environment variable, not a
session variable):

```bash
QUACKAPI_SRC='path/to/app.py' \
  /opt/homebrew/bin/duckdb -unsigned /tmp/qmig.db \
  -c ".read migrate/migrate_fastapi.sql" \
  -c ".read migrate/COVERAGE.sql"
```

**Note:** There is no `SET VARIABLE` in this tool. The source path is read via
`getenv('QUACKAPI_SRC')` at query time — a launch-time immutable that does not
affect DuckDB session state.

## What the tool auto-generates

For each route it can fully resolve, the tool emits:

1. `INSERT INTO routes SELECT * FROM register_route(...)` with the correct
   HTTP method, resolved path (including APIRouter prefix + include_router prefix),
   and a `TODO` placeholder for the handler SQL.

2. `INSERT INTO param_schema VALUES (...)` for each parameter, pre-classified as:
   - `path` — name appears as `{name}` in a path segment
   - `body` — param type is a BaseModel or SQLModel subclass (direct or one-level indirect)
   - `query` — everything else (primitives with or without defaults)

3. Required/optional flag from the AST: `typed_parameter` = required,
   `typed_default_parameter` = has a default → optional.

4. Type mapping: `str`→`string`, `int`→`int`, `float`→`float`, `bool`→`bool`,
   anything else (model types) → `struct`.

5. A `response_model=` annotation in the `[MIGRATED]` / `[NEEDS_REVIEW]` comment
   line when the decorator declares one.

6. A `501 catch-all` route stub at the bottom for paths not yet migrated.

### What the tool does NOT generate

**Handler bodies.** The handler SQL column is intentionally left as a `-- TODO`
placeholder. The tool generates the routing and validation skeleton; you write
the DuckDB SQL that executes per request. This is the correct boundary: handler
logic is business logic, not shape inference.

## APIRouter prefix resolution

The tool resolves two levels of prefix:

```python
items_router = APIRouter(prefix="/items")   # own_prefix = /items
@items_router.get("/{item_id}")             # deco_path  = /{item_id}
def get_item(item_id: int): ...             # full_path  = /items/{item_id}  ✓

app.include_router(users_router, prefix="/users")  # extra_prefix = /users
@users_router.get("/{user_id}")             # deco_path  = /{user_id}
def get_user(user_id: int): ...             # full_path  = /users/{user_id}  ✓
```

Both levels compose: if the router has its own prefix AND an include_router prefix,
both are concatenated. Double slashes are suppressed.

Prefixes that are dynamic values (e.g. `prefix=settings.API_V1_STR`) cannot be
resolved — the tool emits the router's own prefix only.

## Model detection

The tool recognizes:

- **Direct** — `class Foo(BaseModel): ...` or `class Foo(SQLModel): ...`
- **One-level indirect** — `class Bar(Foo): ...` where `Foo` is a direct model
  (covers SQLModel patterns like `UserCreate(UserBase)`, `ItemPublic(ItemBase)`)

Models found anywhere in the glob (not just the route file) count as "local" —
if you pass `app/**/*.py`, models defined in `app/models.py` are visible to
routes in `app/api/routes/items.py`.

## Dependency-injection (DI) alias filtering

Module-level type aliases of the form `SomeAlias = Annotated[T, Depends(...)]`
are detected automatically and excluded from `param_schema`. This covers common
FastAPI patterns like:

```python
SessionDep = Annotated[Session, Depends(get_db)]
CurrentUser = Annotated[User, Depends(get_current_user)]
```

Params typed with these aliases are not data params — they carry no request
payload — so they must not appear in `param_schema`. The coverage report lists
detected aliases in the `DI ALIASES` section.

Inline `Annotated[..., Depends()]` expressions used directly in a function
signature (not via a named alias) are NOT auto-detected. They will be flagged
NEEDS_REVIEW as an imported type.

## Coverage taxonomy

Every route in the source either appears in one of these buckets or is counted
in NOT_DETECTED. Nothing silently drops.

| Status | Meaning |
|--------|---------|
| `MIGRATED` | Clean. Route + param stubs emitted. Write handler SQL. |
| `NEEDS_REVIEW` | Partial. Registration stub emitted but flagged. See reasons below. |
| `NOT_DETECTED` | Invisible to the decorator scan. Must be migrated manually. |

The coverage report also includes a **ground-truth self-check** (Section 0): it
counts HTTP-verb decorators in the AST directly and compares against the number
of routes emitted. A `Delta = 0` proves nothing was missed or double-counted.

### NEEDS_REVIEW reasons

- **Imported body model** — a param's type (e.g. `payload: ExternalModel`) is
  not found in any file in the glob. The tool cannot extract fields. Emits a
  commented-out `param_schema` stub and a clear explanation. Fix: either include
  the model's file in the glob, inline the model, or add the `param_schema` rows
  manually.

- **Class-based view (CBV)** — a decorated method lives inside a class body, not
  at module level. The route path and method are extracted correctly, but CBV
  wiring is non-standard. Emits a handler stub; you must verify the path and
  method are correct.

- **Dynamic path** — the first positional argument to the decorator is not a
  string literal (e.g. `@router.get(DefaultActionIds.tags_multi_select)` or an
  f-string). The path cannot be extracted statically. Emits a stub with
  `<dynamic:path>` placeholder.

- **Annotated param** — a function parameter uses `Annotated[T, Header(...)]` or
  a similar FastAPI `Annotated` form inline. The location (header / cookie /
  form / query) is detected from the annotation, but manual verification is
  recommended.

### NOT_DETECTED cases

- **`app.add_api_route(...)`** — dynamic registration, not a decorator. The tool
  counts these and reports file + line number. Migrate manually.

- **`app.mount("/path", sub_app)`** — attaches a whole FastAPI sub-application.
  The tool cannot introspect the sub-app from the mount call alone. Migrate the
  sub-app separately (run the tool against its file), then adjust paths.

  **Note:** routes declared on `sub_app` via `@sub_app.get(...)` ARE detected as
  plain routes (they appear as MIGRATED/NEEDS_REVIEW with their local path, e.g.
  `/ping`). When migrating them, prepend the mount prefix (`/sub`) to get the
  final path (`/sub/ping`). The mount warning in section 5 of the coverage report
  is your cue to do this.

## Production-scale stress test — Netflix/Dispatch

Tested against [Netflix/dispatch](https://github.com/Netflix/dispatch) — a production incident
management app (~40k LOC, 135 Python files in `src/dispatch/**`):

```bash
bash migrate/run_migrate.sh 'path/to/dispatch/src/dispatch/**/*.py'
```

| Metric | Value |
|--------|-------|
| Ground-truth decorators | 291 |
| Tool emitted routes | 291 |
| Delta | 0 (clean) |
| MIGRATED | 158 |
| NEEDS_REVIEW | 133 |
| NOT_DETECTED (add_api_route) | 0 |
| NOT_DETECTED (app.mount) | 3 |

The 133 NEEDS_REVIEW routes are legitimately non-trivial: dispatch uses many custom
SQLModel types imported from the same-file `models.py` (not in scope when running
only `**/*.py` from the views directories), inline `Depends(...)` params, and
`Query(..., alias=...)` params whose wire name differs from the Python name. All
291 routes are accounted for — Delta=0. (Six routes that an earlier revision
emitted as MIGRATED are now NEEDS_REVIEW: they carried `alias=` or a
default-position `Depends()` the old annotation-only scan could not see — they
were silently wrong, not clean.)

## Worked example — official FastAPI template

Tested against the official `full-stack-fastapi-template` backend:
`app/api/routes/{items,login,users,utils,private}.py` + `app/models.py`.

```bash
bash migrate/run_migrate.sh \
  '/path/to/full-stack-fastapi-template/backend/app/**/*.py'
```

### Results (verified)

| Metric | Value |
|--------|-------|
| Ground-truth decorators | 23 |
| Tool emitted routes | 23 |
| Delta | 0 (clean) |
| MIGRATED | 21 |
| NEEDS_REVIEW | 2 |
| NOT_DETECTED | 0 |
| Local models found | 20 (BaseModel + SQLModel, direct + indirect) |
| DI aliases filtered | 3 (SessionDep, CurrentUser, TokenDep) |

### MIGRATED routes (21)

```
GET  /items/                          (response_model=ItemsPublic)
GET  /items/{id}                      (response_model=ItemPublic)
POST /items/                          (response_model=ItemPublic)
PUT  /items/{id}                      (response_model=ItemPublic)
DELETE /items/{id}
POST /login/test-token                (response_model=UserPublic)
POST /password-recovery/{email}
POST /reset-password/
POST /password-recovery-html-content/{email}
POST /private/users/                  (response_model=UserPublic)
GET  /users/                          (response_model=UsersPublic)
POST /users/                          (response_model=UserPublic)
PATCH /users/me                       (response_model=UserPublic)
PATCH /users/me/password              (response_model=Message)
GET  /users/me                        (response_model=UserPublic)
DELETE /users/me                      (response_model=Message)
POST /users/signup                    (response_model=UserPublic)
GET  /users/{user_id}                 (response_model=UserPublic)
PATCH /users/{user_id}                (response_model=UserPublic)
DELETE /users/{user_id}
GET  /utils/health-check/
```

### NEEDS_REVIEW routes (2 — both correct)

```
POST /login/access-token
  — form_data: Annotated[OAuth2PasswordRequestForm, Depends()]
    Inline Annotated+Depends, not a named alias; OAuth2PasswordRequestForm
    is from fastapi.security (outside the glob).

POST /utils/test-email/
  — email_to: EmailStr
    EmailStr is from pydantic.networks (outside the glob).
```

Both are genuinely unresolvable from the AST alone. The tool flags them
correctly rather than silently emitting wrong SQL.

## Sample outputs (regression-verified)

### simple.py — all MIGRATED

```
MIGRATED  GET  /
MIGRATED  GET  /items/{item_id}    params: item_id (path, int, required), verbose (query, bool, optional)
MIGRATED  GET  /search             params: q (query, string, required), limit (query, int, optional), offset (query, int, optional)
MIGRATED  POST /items              params: body (body, struct, required) [CreateItemBody]
MIGRATED  PUT  /items/{item_id}    params: item_id (path, int, required), body (body, struct, required)
MIGRATED  DELETE /items/{item_id}  params: item_id (path, int, required)
```

Model extracted: `CreateItemBody` → name (str, required), price (float, required), in_stock (bool, optional)

### with_router.py — all MIGRATED, prefix resolved

```
MIGRATED  GET  /items/            (items_router prefix /items + deco path /)
MIGRATED  GET  /items/{item_id}   (items_router prefix /items + deco path /{item_id})
MIGRATED  POST /items/            (items_router prefix /items + deco path /)
MIGRATED  GET  /users/{user_id}   (users_router, include_router prefix /users)
MIGRATED  POST /users/            (users_router, include_router prefix /users)
```

### hard.py — NEEDS_REVIEW and NOT_DETECTED correctly flagged

```
NEEDS_REVIEW  POST /submit       submit        — imported body model: ExternalModel
MIGRATED      GET  /status       status        — clean
MIGRATED      POST /local        create_local  — clean (LocalModel is locally defined)
NEEDS_REVIEW  GET  /cbv/items    list_items    — CBV: method on class body
NEEDS_REVIEW  POST /cbv/items    create_item   — CBV: method on class body
MIGRATED      GET  /ping         sub_ping      — from sub_app (see mount warning)

NOT_DETECTED  /dynamic/{item_id}  add_api_route  hard.py:48
NOT_DETECTED  /dynamic            add_api_route  hard.py:51
NOT_DETECTED  /sub                app.mount      hard.py:74
```

## Reproduce any of the above

```bash
cd /path/to/quackapi

# run_migrate.sh (recommended — handles env var setup)
bash migrate/run_migrate.sh migrate/samples/hard.py

# Manual (set QUACKAPI_SRC yourself)
QUACKAPI_SRC='migrate/samples/hard.py' \
  /opt/homebrew/bin/duckdb -unsigned -csv /tmp/test.db \
  -c ".read migrate/migrate_fastapi.sql" \
  -c ".read migrate/COVERAGE.sql"
```

## File layout

```
migrate/
├── migrate_fastapi.sql   — extractor + registration SQL emitter
├── COVERAGE.sql          — safety report (run in same session after migrate_fastapi.sql)
├── run_migrate.sh        — shell wrapper: sets QUACKAPI_SRC env var, runs both SQLs
├── README.md             — this file
└── samples/
    ├── simple.py         — plain decorators, one BaseModel, no routers
    ├── with_router.py    — APIRouter + prefix + include_router
    └── hard.py           — imported model, add_api_route, CBV, app.mount sub-app
```

## Known remaining gaps

The following are documented limitations, not bugs:

- **Dynamic prefix values** (`prefix=settings.API_V1_STR`) — not resolvable from
  the AST. The outer-level mount prefix in `app/main.py` is a settings lookup and
  cannot be resolved. Routes inherit only the locally-extractable prefixes.

- **Inline `Annotated[T, Depends()]`** in function signatures — not auto-detected
  as DI. Only named module-level aliases are found. Inline uses land as
  NEEDS_REVIEW with a clear message.

- **Two-or-more-level indirect model inheritance** — `class C(B)` where `B(A)` and
  `A(SQLModel)` would require a second transitive pass. Only one level of
  indirection is resolved. Third-level subclasses show as imported models.

- **`response_class=` vs `response_model=`** — the tool captures `response_model`
  only. `response_class=HTMLResponse` is not surfaced (it has no schema impact).

- **Constraint kwargs the runtime does not enforce** — `le=`/`ge=` with literal
  numeric values are pulled through into `param_schema.constraint_json` (the
  quackapi runtime enforces exactly these two), for both `Annotated[int,
  Query(le=100)]` and the legacy `n: int = Query(le=100)` form. Any other
  constraint-family kwarg (`gt`, `lt`, `min_length`, `max_length`, `pattern`,
  `regex`, `multiple_of`, `alias`, or an `le`/`ge` whose value is not a plain
  literal, e.g. negative) forces the route to NEEDS_REVIEW with the kwargs
  named — emitting MIGRATED while dropping a constraint would validate weaker
  than the FastAPI original. `alias=` is in the review list because it renames
  the wire parameter. Ellipsis defaults (`= Query(...)`) are recognized as
  required, and path params are always emitted `required=true`.

- **`list[str]` and generic types** — `list[str]` is not in the primitive set and
  shows as an imported model unless it matches a local class. For list-typed
  query params, add a `param_schema` row manually with `'query'` location.
