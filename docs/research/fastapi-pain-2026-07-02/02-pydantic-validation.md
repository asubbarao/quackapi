# Pydantic/FastAPI Validation Pain Points — Research 2026-07-02

> Lens: validation, serialization, typing. Scope: real GitHub issues, community threads, post-mortems.
> Purpose: inform quackapi's "typed DB columns as the type system" thesis.

---

## Pain Point 1: Pydantic v1 → v2 Migration Churn

**Frequency/Severity:** CRITICAL / Industry-wide. Landed June 30, 2023; FastAPI 0.126.0 (December 2025) dropped v1 entirely, forcing every app and every *library that wraps pydantic* to migrate simultaneously.

**Root cause (technical):** v2 is not a patch — it is a near-complete rewrite into a Rust core (`pydantic-core`), shipping a new public API. Every custom type's `__get_validators__` hook became `__get_pydantic_core_schema__`. `class Config:` became `model_config = ConfigDict(...)`. `@validator` / `@root_validator` became `@field_validator` / `@model_validator`. `parse_obj` / `parse_raw` became `model_validate` / `model_validate_json`. `from_orm` folded into `model_validate(obj, from_attributes=True)`. Models are no longer equal to their `dict()` representations. The Rust binary `pydantic_core` must be compiled for every Python/OS/arch combination, introducing binary wheel supply-chain complexity and occasional install failures on non-mainstream toolchains (pydantic-core#1366, pydantic-core#1202).

The ecosystem was stuck writing dual-compatibility shims for ~18 months because there is no clean way to support both versions in a single package. The Pydantic team explicitly acknowledged: *"The transitions from Pydantic V1 to V2 has been and will be painful for some users."* FastAPI itself kept a v1 compat layer (`PYDANTIC_V1` env var) until it broke the promise in late 2025.

**Classification: A — Architectural crack quackapi beats.**  
quackapi has no version history. The "schema" is `information_schema.columns` + a constraint table. Upgrading DuckDB does not rename your validation API. There is no ecosystem of third-party types registered with quackapi that all break simultaneously. The absence of a Python validator plugin system is not a limitation here — it is immunity.

**quackapi angle:**  
`TRY_CAST(value AS declared_type)` returns NULL on failure — that behavior is stable across DuckDB versions. The constraint table (min/max/enum/required) is plain SQL rows. No Rust binary, no custom type registry, no migration guide. The tradeoff: you cannot register custom Python types with quackapi's validator. If you want to validate `MoneyAmount` as a distinct type, it's `DECIMAL(12,2)` with a check constraint — good enough for 95% of cases, but you lose the semantic label.

---

## Pain Point 2: Cryptic, Verbose Error Output — The `url`, `input`, `ctx` Problem

**Frequency/Severity:** HIGH / Every Pydantic v2 user hits this on day 1.

**Root cause (technical):** Pydantic v2 changed `ValidationError.errors()` to return, for each error:
```json
{
  "loc": ["body", "items", 0, "price"],
  "msg": "Input should be a valid number",
  "type": "float_parsing",
  "input": "banana",
  "url": "https://errors.pydantic.dev/2.4/v/float_parsing",
  "ctx": {"error": "unable to parse string as a number"}
}
```
Three problems:
1. **`url`** — an external docs link leaks in every API error response, which is noisy, inconsistent, and a potential information disclosure. Removing it requires either `os.environ['PYDANTIC_ERRORS_INCLUDE_URL'] = '0'` or a custom exception handler. FastAPI fixed automatic exclusion in 0.110.2 — but only for `url`; `input` and `ctx` remain user-territory (fastapi#10934, fastapi#10352, pydantic#7485).
2. **`ctx`** can contain a live `ValueError` Python object — not JSON-serializable. FastAPI's default `jsonable_encoder(exc.errors())` will blow up if `ctx` is present and unfiltered.
3. **`loc` is a tuple of `Union[int, str]`** — but FastAPI's OpenAPI generator typed it as `array[string]`, so the spec and the actual response don't match (fastapi#3621, fastapi#8767). Integer path segments (array index in a nested model) don't roundtrip through the schema.

As of mid-2024, FastAPI PR #13657 proposed `include_error_input` / `include_error_url` flags on `FastAPI()` and `APIRouter()` — these are still not in stable as of research date.

**Classification: B — Execution gap quackapi can do better.**  
The 422 shape is not the problem. The problem is Pydantic stitching debug info (live Python exceptions, URLs to docs) into API-boundary error responses with no clean global opt-out.

**quackapi angle:**  
quackapi builds the 422 response from scratch from query results — it emits exactly `{"loc":[...],"msg":...,"type":...}` and nothing else. No leaked debug objects, no `url`, no `ctx`. The `loc` array uses the declared column name (string) — integer array-indices are only emitted for `ARRAY` columns. `msg` comes from a template per error type: `"value_required"`, `"type_error.int"`, `"value_error.number.not_ge"`, etc. — all strings, all JSON-safe. No custom exception handler needed.

The honest gap: quackapi's error messages are currently templated strings, not multi-language or contextual. Pydantic's `ctx` exists to carry the *reason* for a custom validator failure (e.g., `{"discriminator": "payment_type"}`). quackapi has no equivalent. Custom error context needs to be baked into constraint-table `message` column — works, but less dynamic.

---

## Pain Point 3: Import / Startup Performance Regression

**Frequency/Severity:** HIGH / Particularly acute for CLI apps, Lambda cold starts, test suites.

**Root cause (technical):** The Rust pydantic-core binary is loaded at import. Combined with Python-side model construction (schema compilation), large applications or generated model collections pay significant startup costs:
- Simple CLI: 0.8s (v1) → 1.9s (v2) just for `--help`
- GraphQL client with ~1,900 models: 6.99s on v2.7.0
- Test suites (AWS Powertools): 3 minutes (v1) → 11 minutes (v2)
- FastAPI import: 336,969 function calls (v1) → 1.3 million (v2)
- Kubernetes Python client: never upgraded due to startup time impact

The Rust compilation of schemas happens at class definition time, not at first use. The Python-side `model_rebuild()` call for forward references compounds this — every circular/forward-ref model must be explicitly rebuilt after all referenced types are defined.

**Classification: A — Architectural crack quackapi beats.**  
quackapi has no schema compilation step. The "schema" is a SQL `CREATE TABLE` statement or `information_schema.columns`. Startup = DuckDB `attach` + loading the constraint table. No class hierarchy, no Rust schema compilation, no `model_rebuild()`.

**quackapi angle:**  
`TRY_CAST` is evaluated at query time, not at startup. The constraint table is an ordinary DuckDB scan. Cold-start cost for quackapi is DuckDB process + extension load (~100ms), not model compilation. The tradeoff: there is no "compile-time" schema check in the Python host process — if you misspell a column name in a route definition, you find out at request time, not import time. Pydantic actually catches schema errors earlier (at class body execution), which is a genuine safety advantage for large teams.

---

## Pain Point 4: `json_encoders` Removal and Serialization Semantic Drift

**Frequency/Severity:** HIGH / Every project using `Decimal`, `datetime`, or MongoDB `ObjectId` hit this wall.

**Root cause (technical):** Pydantic v1 allowed `class Config: json_encoders = {Decimal: str, datetime: lambda dt: dt.isoformat()}` — a single point of truth for how any instance of those types serialized to JSON, across every model in the application. v2 deprecated this and requires per-field or per-model `@field_serializer` decorators (pydantic#6697, pydantic#6375, pydantic#6726). The migration guide says "use `@field_serializer`" but that requires touching every model, and it's per-field not per-type.

Concrete serialization behavior changes:
- `Decimal` → in Python mode, `Decimal` object; in JSON mode, **string** (v1 defaulted to float). Switching between modes silently changes your API response shape.
- `datetime` → ISO 8601 string in JSON mode, but FastAPI's `jsonable_encoder` converts it differently depending on whether you go through `response_model` or `JSONResponse` directly.
- `UUID` → string in JSON mode, `UUID` object in Python mode.
- Double-serialization trap: FastAPI calls `jsonable_encoder()` before passing to `JSONResponse`, which can re-process a `model_dump(mode='json')` result and apply different rules a second time.

Performance regressions were sharp: upgrading from 2.9.1 to 2.9.2 caused 75,000× slowdown on large nested models with self-referencing links (pydantic#10709). A version bump between 2.0.3 and 2.1.1 doubled average FastAPI response time from 250ms to 450ms (pydantic#7001).

**Classification: A — Architectural crack quackapi structurally avoids.**  
DuckDB has exactly one serialization path per type: `DECIMAL` serializes as a number in JSON (configurable by DuckDB's JSON dialect); `TIMESTAMP` serializes as ISO 8601 string; `UUID` serializes as `"xxxxxxxx-xxxx-..."`. This is determined by DuckDB's `json_serialize_sql` / JSON type system, not by Python object dispatch. There is no "Python mode vs JSON mode" split. There is no risk of a version bump changing how a `DECIMAL` column renders.

**quackapi angle:**  
quackapi reads from DuckDB, which produces JSON-safe types directly (via `json_object()` / `struct_pack` / arrow). The response serializer does one pass. No `jsonable_encoder`, no mode-switching, no `@field_serializer` decoration. The tradeoff: if you need custom serialization (e.g., `Decimal` as a formatted string with currency symbol), you express it as a DuckDB `PRINTF` or `FORMAT` expression in the view definition — SQL, not Python decorators, which is actually more composable for data-heavy APIs.

---

## Pain Point 5: DTO Boilerplate — Three-Model Problem

**Frequency/Severity:** HIGH / Structural property of the FastAPI + SQLAlchemy stack.

**Root cause (technical):** A typical FastAPI resource requires:
1. **SQLAlchemy ORM model** — maps to the DB table, carries relationships.
2. **Request Pydantic model** (`UserCreate`) — what the client sends; excludes server-generated fields like `id`, `created_at`.
3. **Response Pydantic model** (`UserOut`) — what the API returns; excludes `hashed_password`.

Three separate class definitions, all carrying overlapping field lists. If you add a column, you update 3 files. SQLModel (tiangolo's own band-aid library) attempts to merge 1 and 3, but it's been notoriously slow to reach stability and introduces its own quirks. The FastAPI docs explicitly acknowledge the duplication: *"Reducing code duplication is one of the core ideas in FastAPI"* — then spend three pages explaining how to use inheritance to manage it.

**Classification: A — Architectural crack quackapi's DB-centric model genuinely eliminates.**  
In quackapi, the table definition *is* the schema. Request validation is `TRY_CAST` against `information_schema.columns`. Response shape is the `SELECT` projection of the same table. Write a view that excludes `hashed_password` from responses — that is a single SQL `CREATE VIEW`. Add a column to the table — it appears in requests and responses automatically, constrained by the same type system. No class to update.

**quackapi angle:**  
`CREATE TABLE users (id UUID DEFAULT gen_random_uuid(), name VARCHAR NOT NULL, hashed_password VARCHAR)` + `CREATE VIEW users_public AS SELECT id, name FROM users` replaces UserCreate + UserOut + ORM model. The constraint table handles `required` / `min_length` / pattern. The tradeoff: the DB schema is necessarily the single point of truth — you cannot have request and response shapes that structurally diverge (e.g., `request.tags: string[]` → `response.tags: Tag[]` with resolved objects). For that you need a projection view with a JOIN, which is SQL-native but more verbose for object-graph APIs.

---

## Pain Point 6: Complex Nested / Discriminated Union / Circular Model Pain

**Frequency/Severity:** MEDIUM-HIGH / Hits teams building event-driven or polymorphic APIs.

**Root cause (technical):**
- **Discriminated unions**: require a `Literal` discriminator field. If an unknown value arrives (evolving API, external partner payload), Pydantic raises a `ValidationError` rather than falling back gracefully. `union_mode='left_to_right'` (v1 default) tried types in order — slow on deep models, confusing errors on mismatch. v2's `union_mode='smart'` (new default) is better but still produces `Union[A, B, C]` errors where the user gets three parallel error trees, all for the same input value (pydantic#8789, pydantic#10409).
- **Circular / self-referential models**: require explicit `model_rebuild()` after all forward-ref types are defined. Forgetting it produces confusing `PydanticUserError: `A` is not fully defined; you should define `B`, then call `A.model_rebuild()``. Timing it wrong (call before a subclass is defined) causes silent schema corruption.
- **Generic models**: `Model[T]` with forward refs inside `T` requires multi-step rebuild that the docs underspecify (pydantic#8789 long thread on ordering).

**Classification: C — Deliberate Pydantic tradeoff. Respect it; don't claim to beat it.**  
Discriminated unions and polymorphic payloads are a genuinely hard problem. Pydantic's union machinery — despite the pain — gives you rigorous type-narrowing with error attribution per candidate. quackapi is not solving polymorphic union validation. A `CHECK` constraint column can implement a discriminator (`CHECK (event_type IN ('click','view','purchase'))`), but it cannot dispatch to different sub-schemas based on that value. This is a real gap.

**quackapi angle:**  
Flat, strongly-typed schemas are quackapi's sweet spot. `STRUCT` types in DuckDB can represent nested objects, and `TRY_CAST(value AS STRUCT(price DECIMAL, qty INT))` validates nested input. But DuckDB does not have discriminated unions at the type level — you would need to implement dispatch in a `CASE WHEN` and validate each branch separately, which requires application-level routing, not schema-level routing. Honest assessment: quackapi punts on polymorphic union validation. Document it as out-of-scope (flat schemas only) rather than claim parity.

---

## Pain Point 7: Coercion Surprises in Lax Mode (Default)

**Frequency/Severity:** MEDIUM / Insidious; causes silent data corruption in production.

**Root cause (technical):** Pydantic's lax mode (default, `strict=False`) applies "intuitive" coercions:
- `"42"` → `42` for `int` fields (from form data, query strings)
- `1` / `0` / `"true"` / `"false"` → `True` / `False` for `bool` fields — `bool` inherits from `int` in Python so `isinstance(1, bool)` is `True`
- `"2024-01-01"` → `datetime(2024, 1, 1)` for `datetime` fields
- Empty string `""` → validation error for `int`, but NOT for `str` (passes through as `""`)

The bool story is the most dangerous: pydantic#579 (opened in 2019, 350+ thumbs-up) documents that `bool` fields accept `0`, `1`, `"true"`, `"false"`, `"yes"`, `"no"`, `"on"`, `"off"` in lax mode. A database that receives `is_active: "on"` coerced to `True` has just silently accepted a string through a boolean gate. In v2, `strict=True` fixes this but is not the default.

The confusion deepens: v1's `strict` was field-level only; v2 introduced model-level `model_config = ConfigDict(strict=True)` and per-validation-call `strict=True` — three different granularities with different inheritance rules.

**Classification: C — Deliberate tradeoff. Lax mode exists for real reasons.**  
HTML form data and query parameters are always strings. An API that accepts `?limit=10` needs to coerce `"10"` → `10`. Strict mode breaks this common case. Pydantic's lax mode is a reasonable default for web APIs. The pain is in the *surprise* — users don't realize `bool` coercion is so wide.

**quackapi angle:**  
`TRY_CAST('10' AS INTEGER)` → `10`. `TRY_CAST('banana' AS INTEGER)` → `NULL` (captured as type error). DuckDB's casting rules are documented and deterministic. The bool story: `TRY_CAST('true' AS BOOLEAN)` → `true`; `TRY_CAST('on' AS BOOLEAN)` → `NULL` in DuckDB — stricter than Pydantic lax by default, which is arguably better behavior. No ambiguous `"yes"` / `"on"` / `"off"` coercion. The constraint table's `type` column encodes exactly which DuckDB cast applies — transparent, reproducible, greppable.

---

## Pain Point 8: `response_model` Serialization Overhead on Large Lists

**Frequency/Severity:** MEDIUM / Surfaces at scale (> 1000 items in a list response).

**Root cause (technical):** When a FastAPI route declares `response_model=List[ItemOut]`, FastAPI:
1. Takes the return value (often a SQLAlchemy query result).
2. Calls `jsonable_encoder()` on it — converts ORM objects to dicts.
3. Validates the dicts against `ItemOut` via Pydantic.
4. Calls `model.model_dump(mode='json')` to get JSON-safe primitives.
5. Passes to `JSONResponse` which `json.dumps()` it.

That's two serialization passes. Benchmarks show Pydantic v2 `model_dump()` at 28.6µs per object — for a list of 10,000 objects that's 286ms just in `model_dump` before network I/O. Validation and serialization can account for 40% of request latency on complex response models. Workaround: `response_model=None` + manually return `JSONResponse(content=data)` — but that bypasses all response validation.

**Classification: B — Execution gap.**  
The double-serialization path is an implementation choice in FastAPI, not a fundamental property of typed APIs.

**quackapi angle:**  
quackapi returns the DuckDB JSON output directly. The query produces a JSON-typed result via `json_object()` or `to_json()` — one pass, no Python object construction per row, no second validation pass. The tradeoff: response filtering (excluding fields per user role) must be done in the `SELECT` projection or a VIEW, not by runtime `response_model` field exclusion. This is actually more explicit but requires SQL authorship.

---

## What Pydantic Does That a Pass-Through Typed DB Genuinely Cannot

This is the most important section. Be honest.

### 1. Cross-Field / Conditional Validation

Pydantic's `@model_validator(mode='after')` can express:
```python
@model_validator(mode='after')
def check_password_match(self) -> 'UserCreate':
    if self.password != self.confirm_password:
        raise ValueError('passwords do not match')
    return self
```

DuckDB `CHECK` constraints can reference multiple columns of the *same row*:
```sql
CHECK (password_hash IS NOT NULL OR oauth_provider IS NOT NULL)
```
But `CHECK` constraints cannot compare across *request fields that don't all live in the same table row* — e.g., `new_password != old_password` where `old_password` comes from the request body, not the DB. This requires a JOIN or application-level fetch, neither of which fits in a schema-level TRY_CAST pass.

**quackapi gap**: cross-field validation (confirm_password, date_start < date_end, at_least_one_of) must be implemented as a post-cast application layer, not in the constraint table. quackapi needs a "cross-field rule" concept that is absent from the current design.

### 2. Context-Aware Validation

Pydantic allows passing a `context` dict into validation:
```python
model.model_validate(data, context={'user_id': current_user.id, 'tenant': tenant_slug})
```
Validators can then read `info.context` to apply rules like "this field is only required for premium users." DuckDB's schema constraints have no concept of request context — they are structural invariants, not business-rule conditionals.

**quackapi gap**: any validation that depends on *who is asking* or *what else is in the system* (auth-level-dependent required fields, tenant-specific limits) cannot be expressed in the constraint table. It must be pre-flight application logic before the DuckDB validation pass.

### 3. Computed / Derived Fields

`@computed_field` lets Pydantic include a property as a serialized response field:
```python
@computed_field
@property
def full_name(self) -> str:
    return f"{self.first_name} {self.last_name}"
```
DuckDB can do this trivially in a `SELECT`:
```sql
SELECT first_name || ' ' || last_name AS full_name FROM users
```
**Not a gap** — SQL computed columns (view expressions) are cleaner than Python properties for data-layer transformations. This is actually where quackapi wins.

### 4. Custom Python Type Semantics (EmailStr, AnyUrl, PhoneNumber, etc.)

Pydantic has `EmailStr`, `AnyUrl`, `HttpUrl`, `IPvAnyAddress`, `PaymentCardNumber`, `PhoneNumber` (via pydantic-extra-types) with real parsing logic (RFC compliance, DNS lookups for email, URL component extraction). DuckDB's `VARCHAR` with a regex CHECK constraint approximates this but:
- No DNS MX lookup for email validation.
- No URL component extraction (scheme, host, path as validated sub-fields).
- No Luhn check for card numbers.
- No E.164 parsing with country-code normalization.

**quackapi gap**: semantic type validation beyond structural type-checking requires either (a) DuckDB UDFs (the C++ extension path), or (b) a pre-cast application layer. The C++ extension path is viable for quackapi's thesis — `email_validate(value)` as a DuckDB scalar UDF is feasible. But it doesn't come for free; it's user-authored.

### 5. Transform-On-Validate (Mutations During Parsing)

Pydantic validators can *transform* input, not just check it:
```python
@field_validator('tags', mode='before')
@classmethod
def split_tags(cls, v):
    if isinstance(v, str):
        return v.split(',')
    return v
```
DuckDB's TRY_CAST does not mutate — it casts or returns NULL. If the client sends `"tags": "sports,news"` and you want `["sports","news"]`, you need an application-layer pre-processor before the DuckDB pass, or a SQL expression in the route handler.

**quackapi gap**: input transformation (normalization, coercion of non-standard shapes) is not in the constraint table. quackapi's current design is validate-or-reject, not validate-and-transform. This is a deliberate tradeoff (simpler, auditable) but it means quackapi handles fewer real-world wire formats without preprocessing.

### 6. Recursive / Self-Referential Schema Validation

A tree node `class Node(BaseModel): children: List['Node']` validates arbitrarily deep JSON trees against a recursive schema. DuckDB's `STRUCT` and `LIST` types are typed but not self-referential — you cannot define `STRUCT(id INT, children LIST(STRUCT(...)))` to arbitrary depth. Validating recursive JSON payloads in quackapi requires either (a) bounding the depth in the schema or (b) treating the children field as `JSON` (unvalidated blob).

**quackapi gap**: deep recursive payload validation is not possible in the constraint table. For tree/graph APIs, quackapi reduces to "is it valid JSON?" — structural type validation cannot enforce invariants at depth N.

### 7. Rich Error Attribution for Complex Schemas

When a `List[Union[A, B, C]]` fails validation in Pydantic, it produces a structured error with the exact array index and which union branch failed, with per-candidate error trees. DuckDB's row-level `TRY_CAST` returns NULL or a value — it has no mechanism to attribute "row 3 of your input array, field `price`, union branch B failed." For quackapi's flat schema, error attribution is exact. For nested `LIST(STRUCT(...))` inputs, error attribution degrades: quackapi can say "element 3 of `items` failed" (by iterating), but cannot produce per-union-branch failure trees.

**quackapi gap**: error granularity for deeply nested or polymorphic inputs is coarser than Pydantic's. Flat schemas: quackapi wins on clarity. Complex nested/union schemas: Pydantic wins on attribution precision.

---

## Summary Table

| Pain Point | Class | quackapi Position |
|---|---|---|
| v1→v2 churn / Rust rewrite | A | Immune — no validator plugin system, no version migration |
| Verbose/cryptic error output (url, ctx, loc type) | B | Native clean 422 — emit only loc/msg/type |
| Import/startup slowdown from schema compilation | A | No schema compile — costs move to query time |
| json_encoders removal / serialization semantic drift | A | DuckDB type system owns serialization — one path |
| Three-model DTO boilerplate | A | Table = schema = response shape — one definition |
| Discriminated union / circular model pain | C | Punt — flat schemas only; document honestly |
| Coercion surprises in lax mode | B | DuckDB casts are strict-by-default, deterministic |
| response_model double-serialization overhead | B | One-pass JSON from DuckDB — no validation on output path |
| **Cross-field validation** | — | **GAP** — needs application-layer supplement |
| **Context-aware validation** | — | **GAP** — constraint table is context-free |
| **Input transformation on parse** | — | **GAP** — validate-or-reject, not validate-and-mutate |
| **Semantic types (Email, URL, PhoneNumber)** | — | **Partial** — C++ UDF extension path viable |
| **Recursive/self-referential schemas** | — | **GAP** — bounded depth only |
| **Union error attribution at depth** | — | **GAP** — degrades for nested LIST(UNION(...)) |

---

## Strategic Read

quackapi's thesis is strongest for **flat, data-heavy, CRUD-shaped APIs** where the DB schema IS the contract. It has genuine architectural wins over Pydantic for: migration stability, startup performance, serialization determinism, DTO consolidation, and error output cleanliness.

The gaps are real and should be disclosed rather than papered over: cross-field rules, context-dependent required fields, input normalization, and deep recursive payloads are things Pydantic handles in Python that quackapi either defers to application code or doesn't handle. These are not bugs in quackapi's design — they are the natural boundary of "DB types as the type system." The honest pitch: quackapi is a better foundation for 80% of REST endpoints (list/get/create/update resources over typed tables). The other 20% — polymorphic, context-sensitive, transform-heavy — still needs application logic either way; quackapi just makes the 80% case dramatically simpler.

---

## References

- [pydantic/pydantic #6748](https://github.com/pydantic/pydantic/discussions/6748) — v2 significantly slower than v1 (startup benchmarks)
- [pydantic/pydantic #10709](https://github.com/pydantic/pydantic/issues/10709) — 75,000× serialization regression 2.9.1→2.9.2
- [pydantic/pydantic #7001](https://github.com/pydantic/pydantic/issues/7001) — JSON serialization performance decrease 2.0.3→2.1.1
- [pydantic/pydantic #6697](https://github.com/pydantic/pydantic/discussions/6697) — Missing json_encoders in v2
- [pydantic/pydantic #6375](https://github.com/pydantic/pydantic/issues/6375) — Support json_encoders in v2
- [pydantic/pydantic #6726](https://github.com/pydantic/pydantic/issues/6726) — Appropriate replacement for json_encoders
- [pydantic/pydantic #7485](https://github.com/pydantic/pydantic/discussions/7485) — How to remove url & input fields from validation error response
- [pydantic/pydantic #8789](https://github.com/pydantic/pydantic/discussions/8789) — Tagged unions with inheritable members
- [pydantic/pydantic #10409](https://github.com/pydantic/pydantic/issues/10409) — Discriminated unions without nested models
- [pydantic/pydantic #6523](https://github.com/pydantic/pydantic/issues/6523) — Consider releasing v2 under different package name
- [pydantic/pydantic #579](https://github.com/pydantic/pydantic/issues/579) — StrictBool (bool coercion, 350+ thumbs-up)
- [fastapi/fastapi #3621](https://github.com/fastapi/fastapi/issues/3621) — loc tuple vs OpenAPI string array mismatch
- [fastapi/fastapi #8767](https://github.com/fastapi/fastapi/discussions/8767) — Pydantic validation error loc OpenAPI spec
- [fastapi/fastapi #9709](https://github.com/fastapi/fastapi/discussions/9709) — FastAPI with Pydantic v2 migration discussion
- [fastapi/fastapi #10934](https://github.com/fastapi/fastapi/discussions/10934) — Allow customizing ValidationError.errors() arguments
- [fastapi/fastapi #10352](https://github.com/fastapi/fastapi/discussions/10352) — Hide input from response upon 422 Validation Error
- [fastapi/fastapi #10954](https://github.com/fastapi/fastapi/discussions/10954) — Response serialization/deserialization performance
- [fastapi/fastapi #13657](https://github.com/fastapi/fastapi/pull/13657) — PR: include_error_input / include_error_url flags
- [pydantic-core #1366](https://github.com/pydantic/pydantic-core/issues/1366) — Installation failure Python 3.12 / Rustc 1.79
- [pydantic-core #1202](https://github.com/pydantic/pydantic-core/issues/1202) — Build failure Rust 1.68.2
- [FastAPI migration guide](https://fastapi.tiangolo.com/how-to/migrate-from-pydantic-v1-to-pydantic-v2/)
- [Pydantic migration guide](https://docs.pydantic.dev/latest/migration/)
- [Home Assistant: Moving to Pydantic v2](https://developers.home-assistant.io/blog/2024/12/21/moving-to-pydantic-v2/)
- [Medium: FastAPI Code Duplication & SQLModel](https://medium.com/@kiplimoboor/fastapi-code-duplication-in-model-handling-how-sqlmodel-saves-the-day-bbd64137e945)
- [Dev.to: FastAPI Mistakes That Kill Performance](https://dev.to/igorbenav/fastapi-mistakes-that-kill-your-performance-2b8k)
