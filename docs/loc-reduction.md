# LOC reduction

## DuckDB 2.0 native-value cleanup (0.2.0 candidate)

- Route parameter metadata uses native `LIST<STRUCT>` values instead of a
  private percent-encoded string format. This also fixes rounded numeric limits.
- Policy column/type arguments use native string lists. Policy identities,
  view checks and scalar-macro lookups use the native catalog API, with caches
  scoped to one request.
- Server/request options share precedence and default handling. Function
  overloads share named argument signatures.
- JSON rows are encoded once during fetch; Arrow projects native vectors into
  the converter. The row matrix and Arrow reconstruction loop are removed from
  those paths. The final HTTP body remains buffered.

Regression coverage lives in the native metadata, policy catalog, option
precedence, large JSON, Arrow chunk and VARIANT SQL suites. Reproduce response
timings with [bench/large_response.sql](../bench/large_response.sql).

## Pass 1 (merged #1 → `785e1df`)

Based on review of the community submission (~11.9k C++): most bulk is
ParserExtension scaffold, not FastAPI attractor layers.

### Done

1. **JWT claims JSON via DuckDB `json_*`** (auth.cpp)  
   Removed hand-rolled recursive JSON parser (~200 lines). Same substrate as
   body field extract in `server.cpp`.  
   **Bug fixed:** `ClaimsToJson` uses shared `QuackapiJsonEscape` (control-byte
   `\u00XX`).

2. **Shared util TU** (`quackapi_util.{hpp,cpp}`)  
   - `QuackapiTrim` — was byte-identical 6×  
   - `QuackapiJsonEscape` — was 2× with auth path incomplete  

## Pass 2 (this branch)

### Done

1. **Enqueue/Nack arity collapse** (`quackapi_queue.cpp`)  
   `EnqueueScalar2/3` → one `EnqueueScalar`; `NackScalar2/3/4` → one
   `NackScalar`. Optional args via `args.ColumnCount()`. Registration still
   lists every overload (DuckDB needs distinct type lists); bodies are shared.

2. **DDL apply *shell* helpers** (not a mega-template over bind payloads)  
   `BindStatusColumn`, `MakeApplyDdlFunction`, `FinishDdlPlan`,
   `EmitOneShotStatus` in `quackapi_util.hpp`.  
   **Why not template×7 Apply\*Bind/Exec?** Those differ by noun fields and
   registry side effects (~not type renames). The review’s “~250 identical
   lines” overstated — only wiring (~status column, plan flags, one-shot emit,
   MakeApply wrapper) was repeated. Domain bind/exec stay explicit.

Net this pass: **~60 LOC** (11 701 → 11 639). Correctness tests green (queue,
routes, group, auth, policy, stream, table_api).

### Still not done

- Split `HandleRequest` (~1 074 lines) for auditability (little LOC win).  
- `from_x` scope: optional separate extension (~11% repo).  
- Optional: `ClaimsToJson` via SQL `to_json` for full substrate symmetry
  (escape path already correct).

## Build

Full tree with real `duckdb` + `extension-ci-tools` submodules.  
`make release` from repo root after `git submodule update --init`.
