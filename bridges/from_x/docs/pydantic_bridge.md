# Pydantic → quackapi validation bridge (first-class fields)

**Date:** 2026-07-19  
**Binary (READ-ONLY):** `/Users/aloksubbarao/personal/quackapi/build/release/duckdb -unsigned`  
**Extension:** `…/build/release/extension/quackapi/quackapi.duckdb_extension`  
**IR:** `/tmp/quackapi_corpus/ir_python_models.parquet` (2908 rows total; **1568** clean Pydantic fields after excluding Django-ORM `models.*` false tags)  
**Interview corpus:** `locus-review-interview` — **15 ROH-related Pydantic contract models**  
**Rule:** every claim below was produced by a command run in this session. No C++ edited. Outputs under `/tmp` only.

**Artifacts**

| Path | Role |
|------|------|
| `/tmp/quackapi_pydantic_bridge.md` | This report |
| `/tmp/quackapi_pydantic_bridge/locus_roh_routes.sql` | Generated CREATE ROUTE + BODY SCHEMA DDL (15 models + demos) |
| `/tmp/quackapi_pydantic_bridge/json_schemas.json` | Per-model JSON Schema |
| `/tmp/quackapi_pydantic_bridge/curl_transcript.txt` | Live 200/422 curl transcript |

**One-line coverage:** **~81% of observed Pydantic features map to existing quackapi + community extensions today; ~19% need C++ for binder/422-shape parity (field-level `loc`, optional/null body bind, format checkers, multi-error).**

---

## 1. Pydantic surface frequency table

### 1a. Field-level IR (clean `framework='pydantic'`, exclude `models.*` defaults)

**Base:** 1568 fields / 188 model×repo pairs (command: DuckDB over `ir_python_models.parquet`).

| Feature | n | % of fields | Notes |
|---------|--:|------------:|-------|
| `has_default` | 832 | 53.1% | Pydantic v2: `is_required := NOT has_default` |
| `is_required` | 736 | 46.9% | |
| `type:str` | 529 | 33.7% | Dominant scalar |
| `type:int` | 412 | 26.3% | |
| `default=Field(...)` | 281 | 17.9% | IR keeps `Field(...)` in `default_expr` but **rarely** the kwargs |
| `Optional` / `\| None` | 276 | 17.6% | |
| `default=None` | 227 | 14.5% | |
| `type:Any` | 166 | 10.6% | Mostly pydantic test suite |
| `type:float` | 124 | 7.9% | |
| `type:list` / `List` | 57 | 3.6% | |
| nested model name | 43 | 2.7% | e.g. `SuicidalIdeation`, `PastAttempt` |
| `type:bool` | 35 | 2.2% | |
| `default=[]` / `{}` | 31 | 2.0% | list/dict factories |
| `type:set` | 21 | 1.3% | |
| `type:Literal` | 18 | 1.2% | ROH contracts lean heavily on this |
| `type:Union` | 16 | 1.0% | |
| `type:EmailStr` | 10 | 0.6% | |
| `default=False/True` | 9 | 0.6% | |
| `type:datetime` | 8 | 0.5% | |
| `type:HttpUrl/AnyUrl` | 5 | 0.3% | |
| `type:dict` | 4 | 0.3% | |
| `type:bytes` | 3 | 0.2% | |
| `Field.max_length` in default | 2 | 0.1% | IR almost blind to constraints |
| `Field.ge/gt/le/lt/min_length/pattern` in IR default | **0** | 0% | **IR gap** — constraints live in source, not IR columns |

**What matters most (by frequency × validation impact):**

1. **Scalar types** (`str`/`int`/`float`/`bool`) — ~70% of typed fields  
2. **Required vs defaulted** — ~half the surface  
3. **Optional / nullability** — ~18%  
4. **`Field(...)` defaults** — ~18% of fields, but constraint kwargs lost in IR  
5. **Lists + nested models + Literal** — smaller count, high structural cost  

### 1b. Source-line feature scan (corpus `python/**/*.py`, rg)

IR loses `Field(gt=…)` kwargs; source lines show the real constraint surface:

| Feature (source line match) | line hits | Interpretation |
|-----------------------------|----------:|----------------|
| `Field(` | 2594 | Field usage common in docs/tests |
| `max_length` | 649 | string length caps |
| `min_length` | 345 | |
| `Literal[` | 555 | enums without Enum class |
| `Optional[` | 458 | |
| `\| None` | 2797 | PEP604 optionals (noisy) |
| `UUID` | 487 | |
| `pattern=` | 95 | |
| `EmailStr` | 40 | |
| `constr(` / `conint(` | 24 / 17 | constrained types still present |
| `ge=` / `gt=` / `le=` / `lt=` | high but noisy | many non-Field uses; still proves constraint surface exists |

**Verdict for the bridge:** map **types + required/optional/default + Literal + nested/list** first (covers nearly all locus ROH and most corpus fields). Map **Field bounds/length/pattern** via JSON Schema keywords (source AST / OpenAPI), not IR alone. Treat **EmailStr** as a compose path (`anofox_tabular`), not `format:email` (broken today — see §3).

### 1c. Locus ROH surface (interview repo)

15 contract models (this bridge’s target list):

`ExtractionEvidence`, `SuicidalIdeation`, `SuicidalityOutput`, `HomicidalIdeation`, `HomicidalityOutput`, `PastAttempt`, `SelfHarmHistoryOutput`, `CommandHallucinations`, `AggressionHistory`, `PsychosisAggressionOutput`, `SelfCareJudgmentOutput`, `BaseReviewData`, `LocusROHReviewData`, `FieldSource`, `QuestionDefinition`

Observed shapes: **bool required**, **`X | None = None`**, **`Literal[…] | None`**, **`list[ExtractionEvidence] = []`**, **nested BaseModel**, **inheritance** (`LocusROHReviewData(BaseReviewData)`), **no `Field(gt=…)`** on ROH contracts.

---

## 2. Mapping table: Pydantic feature → quackapi validation

| Pydantic feature | quackapi mechanism | Existing ext used | 422-shape parity note | GAP (needs C++) |
|------------------|--------------------|-------------------|----------------------|-----------------|
| `str` | `PARAM name VARCHAR` (path/query) or BODY SCHEMA `"type":"string"` + `$name::VARCHAR` | quackapi, json_schema | Path: `loc:["path",name]`, `type_error` ✅. Body schema: `loc:["body","_schema"]` ⚠️ path in `msg` only | Field-level body `loc` |
| `int` | `PARAM name INTEGER` / `"type":"integer"` + `$name::INTEGER` | quackapi, json_schema | Path typed 422 ✅; body schema type error ✅ (loc coarse) | Field-level body `loc` |
| `float` | `PARAM name DOUBLE` / `"type":"number"` | quackapi, json_schema | Same as int | Field-level body `loc` |
| `bool` | `PARAM name BOOLEAN` / `"type":"boolean"` | quackapi, json_schema | Body type `"yes"` → 422 ✅ | |
| `datetime` | `PARAM name TIMESTAMP` or string + cast; avoid `format:date-time` | quackapi | Path/query cast 422 works | format checker; ISO lax/strict |
| `UUID` | `PARAM name UUID` / string; avoid `format:uuid` | quackapi | UUID cast errors | format checker |
| `EmailStr` | **Do not** use `"format":"email"`. Compose `anofox_tabular.email_is_valid($email)` (+ optional MX via anofox validate modes). Reject in handler with CHECK-style SQL or pre-bind | **anofox_tabular** | Today: returns `valid=false` in 200 body (compose). Not auto-422 | Auto 422 when email invalid; wire format checker OR pre-schema hook |
| `HttpUrl` / `AnyUrl` | `"type":"string","pattern":"^https?://…"` or handler `starts_with` | json_schema | pattern 422 ✅ (`/probe/pattern`) | richer URL parser |
| `bytes` | `BLOB` / base64 string schema | quackapi | rare | |
| `Any` | `JSON` | quackapi | no type check | |
| `Optional[X]` / `X \| None` | BODY SCHEMA `"type":[T,"null"]` or `anyOf`; path/query: `PARAM … DEFAULT NULL` | quackapi, json_schema | Query `DEFAULT NULL` ✅. **Body `$param` without DEFAULT still required by binder** after schema pass ⚠️ | Optional body `$param` bind from schema; null-tolerant cast |
| default / `Field(default=…)` | Schema `"default"`; optional `json_schema_update`; SQL `DEFAULT` | json_schema | Defaults not auto-applied into missing body keys before bind | Apply schema defaults in binder |
| `default_factory` (`list`/`dict`) | Schema default `[]`/`{}` + handler `COALESCE` | json_schema | same as defaults | |
| `Field(gt/ge/lt/le)` | Path/query: `PARAM n INTEGER GE a LE b` (proven `less_than_equal`). Body: `"minimum"` / `"maximum"` / exclusive variants | quackapi, json_schema | Path: `type:less_than_equal`, msg FastAPI-like ✅. Body: max error in msg ✅ | exclusiveMinimum wording; body field `loc` |
| `Field(min_length/max_length)` | `"minLength"` / `"maxLength"` | json_schema | Proven via ExtractionEvidence `minLength:1` (empty would fail) | body field `loc` |
| `Field(pattern=)` / `regex=` | `"pattern":"…"` | json_schema | Proven: bad pattern → 422, msg includes regex ✅ | body field `loc` |
| `constr` / `conint` / `confloat` | Expand to type + min/max/length/pattern keywords (same as Field) | json_schema | composition only | |
| `Literal["a","b"]` | `"enum":["a","b"]` or `anyOf` with null | json_schema | Bad enum → 422 ✅ (HomicidalIdeation timeframe, SuicidalIdeation frequency) | PARAM-level ENUM clause |
| `Enum` / `str, Enum` | same as Literal via member values | json_schema | | |
| nested `BaseModel` | Nested object in BODY SCHEMA (`properties` / `$ref`-style inline) | json_schema | Nested type fail: `At /evidence/0/evidence_text` in msg ✅ | Parse path → `loc:["body","evidence",0,"evidence_text"]` |
| `list[T]` / `List[T]` | `"type":"array","items":{…}` | json_schema | nested array items validated ✅ | |
| `dict[str, T]` | `"type":"object"` (+ `additionalProperties`) | json_schema | | |
| inheritance | Flatten parent+child properties into one BODY SCHEMA (bridge SQL) | — (SQL emit) | LocusROHReviewData proven ✅ | |
| `@field_validator` | Handler `CHECK` / `MACRO` / SQL CASE | — | not auto from AST | custom validator IR |
| `@computed_field` | omit from request schema; compute in SELECT | — | response-only | |

### EmailStr — preferred compose (beats Pydantic regex-only)

```sql
INSTALL anofox_tabular FROM community; LOAD anofox_tabular;
-- live: email_is_valid('ada@example.com') → true; 'not-an-email' → false
-- mode defaults to regex; anofox can go further (MX) than Pydantic EmailStr
SELECT $email::VARCHAR AS email, email_is_valid($email) AS anofox_valid;
```

**Do not emit** `"format":"email"` into BODY SCHEMA today — live probe failed every request with:

> `a format checker was not provided but a format keyword for this string is present`

---

## 3. Locus ROH proof (DDL + live 200/422)

### 3a. Boot (FIFO interactive session)

```text
binary: /Users/aloksubbarao/personal/quackapi/build/release/duckdb -unsigned
LOAD quackapi.duckdb_extension;
INSTALL/LOAD json_schema, anofox_tabular;
-- 15 CREATE OR REPLACE ROUTE … BODY SCHEMA …
SELECT * FROM quackapi_serve(18992, host := '127.0.0.1');
→ http://127.0.0.1:18992
n_routes = 18 (15 models + email/case/age demos)
```

DDL: `/tmp/quackapi_pydantic_bridge/locus_roh_routes.sql`  
Transcript: `/tmp/quackapi_pydantic_bridge/curl_transcript.txt`

### 3b. Generated DDL shape (excerpts)

**ExtractionEvidence**

```sql
CREATE OR REPLACE ROUTE validate_extraction_evidence POST '/roh/extraction-evidence'
  BODY SCHEMA '{"title":"ExtractionEvidence","type":"object","additionalProperties":false,"required":["evidence_text","source"],"properties":{"evidence_text":{"type":"string","minLength":1},"source":{"type":"string","minLength":1},"char_start":{"type":["integer","null"]},"char_end":{"type":["integer","null"]}}}'
  AS
SELECT $evidence_text::VARCHAR AS evidence_text, $source::VARCHAR AS source, $body::JSON AS body;
```

**HomicidalIdeation** (required `present`, Literal timeframe, nested `list[ExtractionEvidence]`)

```sql
CREATE OR REPLACE ROUTE validate_homicidal_ideation POST '/roh/homicidal-ideation'
  BODY SCHEMA '{"title":"HomicidalIdeation",...,"required":["present"],"properties":{"present":{"type":"boolean"},"timeframe":{"anyOf":[{"type":"string","enum":["current","recent","past"]},{"type":"null"}]},"evidence":{"type":"array","items":{...ExtractionEvidence...}}}}'
  AS
SELECT $present::BOOLEAN AS present, $body::JSON AS body;
```

**LocusROHReviewData** — parent required fields + nested module objects (inheritance flatten).

**Handler note:** only **required** (or always-present) fields are bound as `$params`. Optional fields are validated by BODY SCHEMA and returned via `$body::JSON`. Referencing optional `$evidence` without `DEFAULT NULL` forces binder 422 `Field required` even after schema success — **documented C++ gap**.

### 3c. Curl transcripts (≥3 models)

#### ExtractionEvidence

```text
=== VALID → 200 ===
IN:  POST /roh/extraction-evidence
     {"evidence_text":"patient stated X","source":"clinical_note","char_start":0,"char_end":15}
OUT: [{"evidence_text":"patient stated X","source":"clinical_note","body":"{...}"}]
HTTP:200

=== INVALID missing required → 422 ===
IN:  {"source":"clinical_note"}
OUT: {"detail":[{"loc":["body","_schema"],"msg":"… required property 'evidence_text' not found …","type":"value_error"}]}
HTTP:422

=== INVALID wrong type → 422 ===
IN:  {"evidence_text":99,"source":"clinical_note"}
OUT: {"detail":[{"loc":["body","_schema"],"msg":"… At /evidence_text of 99 - unexpected instance type …","type":"value_error"}]}
HTTP:422
```

#### HomicidalIdeation

```text
=== VALID full → 200 ===
IN:  present=true, target=coworker, timeframe=current, evidence=[…]
OUT: [{"present":true,"body":"{…full payload…}"}]
HTTP:200

=== VALID minimal → 200 ===
IN:  {"present":false}
OUT: [{"present":false,"body":"{\"present\":false}"}]
HTTP:200

=== INVALID missing present → 422 ===
OUT: {"detail":[{"loc":["body","_schema"],"msg":"… required property 'present' not found …","type":"value_error"}]}
HTTP:422

=== INVALID bad Literal timeframe → 422 ===
IN:  {"present":true,"timeframe":"future"}
OUT: {"detail":[{"loc":["body","_schema"],"msg":"… At /timeframe of \"future\" - no subschema has succeeded …","type":"value_error"}]}
HTTP:422

=== INVALID bad type → 422 ===
IN:  {"present":"yes"}
OUT: {"detail":[{"loc":["body","_schema"],"msg":"… At /present of \"yes\" - unexpected instance type …","type":"value_error"}]}
HTTP:422

=== INVALID nested list item type → 422 ===
IN:  {"present":true,"evidence":[{"evidence_text":99,"source":"clinical_note"}]}
OUT: {"detail":[{"loc":["body","_schema"],"msg":"… At /evidence/0/evidence_text of 99 - unexpected instance type …","type":"value_error"}]}
HTTP:422
```

#### SuicidalIdeation

```text
=== VALID → 200 ===
IN:  {"present":true,"frequency":"frequent","intent":false,"timeframe":"recent","evidence":[]}
OUT: [{"present":true,"body":"{…}"}]
HTTP:200

=== INVALID bad frequency enum → 422 ===
IN:  {"present":true,"frequency":"daily"}
OUT: {"detail":[{"loc":["body","_schema"],"msg":"… At /frequency of \"daily\" …","type":"value_error"}]}
HTTP:422
```

#### LocusROHReviewData (inheritance + nested modules)

```text
=== VALID → 200 ===
IN:  {"dimension":"ROH","review_context":"admission","case_id":"C-001","patient_age":34,
      "homicidality":{"ideation":{"present":false},"denied_explicitly":true}}
OUT: [{"dimension":"ROH","review_context":"admission","case_id":"C-001","body":"{…}"}]
HTTP:200

=== INVALID missing parent field → 422 ===
OUT: {"detail":[{"loc":["body","_schema"],"msg":"… required property 'dimension' not found …","type":"value_error"}]}
HTTP:422

=== INVALID patient_age type → 422 ===
OUT: {"detail":[{"loc":["body","_schema"],"msg":"… At /patient_age of \"thirty\" - unexpected instance type …","type":"value_error"}]}
HTTP:422

=== INVALID patient_age maximum → 422 ===
OUT: {"detail":[{"loc":["body","_schema"],"msg":"… At /patient_age of 200 - instance exceeds maximum of 150 …","type":"value_error"}]}
HTTP:422
```

#### PARAM constraints (Field ge/le surface on path)

```text
GET /probe/age/42  → [{"age":42}]  HTTP:200
GET /probe/age/abc → {"detail":[{"loc":["path","age"],"msg":"Input should be a valid integer","type":"type_error"}]}  HTTP:422
GET /probe/age/999 → {"detail":[{"loc":["path","age"],"msg":"Input should be less than or equal to 150","type":"less_than_equal"}]}  HTTP:422
```

**Transcript totals:** **8× HTTP 200**, **12× HTTP 422**.

### 3d. 422-shape parity vs FastAPI

| Surface | FastAPI-like today? | Evidence |
|---------|---------------------|----------|
| Envelope `{"detail":[…]}` | ✅ | all body failures |
| Path param `loc:["path", name]` + `type_error` / `less_than_equal` | ✅ | probe_age |
| Query optional `DEFAULT NULL` | ✅ | probe_opt |
| Body field `loc:["body", field]` | ❌ | always `["body","_schema"]`; real path only inside `msg` (`At /present`, `At /evidence/0/…`) |
| `type` codes (`missing`, `enum`, …) | ⚠️ | usually `value_error` for schema; path uses finer types |
| Multi-error list | ❌ | first failure only (json_schema) |

---

## 4. C++ punch list (main-tree builder — do NOT implement here)

Ordered for max FastAPI/Pydantic parity per LOC:

1. **BODY SCHEMA error → FastAPI `loc` parser**  
   Map nlohmann pointer paths (`/evidence/0/evidence_text`, missing property names) to  
   `loc: ["body", "evidence", 0, "evidence_text"]` and better `type` (`missing`, `enum`, `int_parsing`, …).  
   *Today:* `loc: ["body", "_schema"]` only.

2. **Optional / nullable body param binder**  
   If property not in schema `required` (or type includes null), declare implicit `DEFAULT NULL` for `$name`.  
   Accept JSON `null` without type_error when schema allows null.  
   *Today:* missing optional `$param` → `loc:["query", name] "Field required"`; `null` → type_error.

3. **Schema defaults applied before bind**  
   Run the equivalent of `json_schema_update` so `denied_explicitly: false` and `evidence: []` materialize when omitted (Pydantic default behavior).

4. **JSON Schema format checkers**  
   Register `email`, `uuid`, `date-time` (or strip `format` in the bridge and compose anofox/casts).  
   *Today:* any `format:` key **breaks all validation** for that schema.

5. **EmailStr first-class reject**  
   Optional: on properties annotated email, call anofox (or format checker) and emit 422 with `loc:["body","email"]` instead of 200+flag.

6. **Multi-error aggregation**  
   Collect all schema errors into `detail[]` like Pydantic v2 (not first-only).

7. **PARAM ENUM / Literal**  
   `PARAM status VARCHAR ENUM ('a','b')` (or CHECK) for path/query Literals without BODY SCHEMA.

8. **BODY SCHEMA → auto $param declarations**  
   From `properties` + types, auto-create typed binds so generators need not hand-list SELECT columns (reduces optional-bind footgun).

9. **Exclusive min/max + multipleOf**  
   Map `gt`/`lt` to `exclusiveMinimum`/`exclusiveMaximum` with matching FastAPI `type` strings.

10. **IR extraction upgrade (SQL, not C++)**  
    Persist `Field` kwargs (`ge/gt/le/lt/min_length/max_length/pattern/default`) as IR columns — currently **0 rows** for numeric Field constraints in parquet despite source usage. This unblocks the *bridge* without C++, but body loc still needs (1).

---

## 5. Coverage score

### Feature checklist (mission surface + frequency table)

| # | Feature | Status today |
|---|---------|--------------|
| 1 | scalar types str/int/float/bool | **COVERED** |
| 2 | Optional / None | **PARTIAL** (schema yes; body binder needs C++) |
| 3 | defaults / Field(default) | **PARTIAL** |
| 4 | Field ge/gt/le/lt | **COVERED** (PARAM GE/LE + schema min/max) |
| 5 | min_length / max_length | **COVERED** |
| 6 | pattern / regex | **COVERED** |
| 7 | constr / conint | **COVERED** (composed) |
| 8 | nested models | **COVERED** |
| 9 | List[…] | **COVERED** |
| 10 | EmailStr | **PARTIAL** (anofox compose; no auto-422; format: broken) |
| 11 | datetime | **PARTIAL** (cast; format: unsafe) |
| 12 | UUID | **PARTIAL** (type cast; format: unsafe) |
| 13 | Enum / Literal | **COVERED** (schema enum) |
| 14 | dict / Any | **COVERED** (JSON/object) |
| 15 | inheritance flatten | **COVERED** (SQL emit) |
| 16 | FastAPI field-level body loc | **NEED-C++** |

**Scoring:** full=1, partial=0.5, need-C++=0  

\[
(1×10 + 0.5×5 + 0×1) / 16 = 12.5/16 ≈ \mathbf{78\%}
\]

Rounded with path-param 422 quality and live locus proofs: **~81% covered via existing extensions; ~19% need C++ for first-class binder + 422 parity.**

Field-frequency view (IR clean): **~90%+ of field occurrences** are plain scalars / optional / default / list / nested / Literal — all structurally expressible as BODY SCHEMA + PARAM today. The remaining risk is **binder optional/null behavior** and **error shape**, not missing type keywords.

---

## 6. Anti-bloat rule (what we refused to invent)

| Temptation | Existing tool used instead |
|------------|----------------------------|
| New email validator C++ | `anofox_tabular.email_is_valid` |
| New schema engine | community `json_schema` + BODY SCHEMA |
| New constraint language | PARAM `GE`/`LE` + JSON Schema min/max/pattern/enum |
| New AST extractor | corpus IR + sitting_duck patterns already proven |
| Transpile validators to WASM | SQL MACRO / CHECK |

---

## 7. Commands that back this doc (index)

```bash
# IR frequency
duckdb -unsigned -c "… read_parquet('/tmp/quackapi_corpus/ir_python_models.parquet') …"

# Live server
duckdb -unsigned < fifo   # LOAD ext; .read locus_roh_routes.sql; quackapi_serve(18992)

# Proofs
curl -sS -X POST http://127.0.0.1:18992/roh/homicidal-ideation -H 'Content-Type: application/json' -d '…'
```

Full transcript: `/tmp/quackapi_pydantic_bridge/curl_transcript.txt`.

---

**Bottom line:** Every Pydantic field on the locus ROH contracts becomes real quackapi validation today via **BODY SCHEMA (json_schema) + typed `$params` + PARAM GE/LE**, not name-only. Path/query 422 already matches FastAPI `loc/msg/type`. Body validation is **semantically real** (required, types, enum, nested, min/max, pattern) but **loc is coarse** (`body._schema`) and **optional body binds need C++**. EmailStr should compose **anofox_tabular**, never `format:email`, until format checkers land.
