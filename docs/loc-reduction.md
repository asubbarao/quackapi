# LOC reduction pass (feat/loc-reduction)

Based on review of the community submission (~11.9k C++): most bulk is ParserExtension scaffold, not FastAPI attractor layers. Real attractor/bloat/bugs fixed here:

## Done

1. **JWT claims JSON via DuckDB `json_*`** (auth.cpp)  
   Removed hand-rolled recursive JSON parser/serializer (~200 lines). Same substrate as body field extract in `server.cpp`.  
   **Bug fixed:** `ClaimsToJson` no longer uses incomplete escape (missing control-byte `\u00XX`); uses shared `QuackapiJsonEscape`.

2. **Shared util TU** (`quackapi_util.{hpp,cpp}`)  
   - `QuackapiTrim` — was byte-identical 6×  
   - `QuackapiJsonEscape` — was 2× with auth path incomplete  

## Not done (next)

- Split `HandleRequest` (~1k lines) for auditability (little LOC win).  
- Template DDL Apply* ×7 (~250 lines).  
- EnqueueScalar2/3 + NackScalar2/3/4 collapse (~80).  
- `from_x` scope: optional separate extension (~11% repo).  

## Build

Use a full tree with real `duckdb` + `extension-ci-tools` submodules (not symlinks).  
`make` from worktree after `git submodule update --init`.
