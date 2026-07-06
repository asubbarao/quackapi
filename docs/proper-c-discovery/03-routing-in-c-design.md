# quackapi Discovery: Routing-in-C Layer Design

**Context (from edges.md Round 3, framework.sql, serve_brain.sql)**

Pure-SQL `handle_request` (the 13-CTE brain with window + list_* lambdas + map_from_entries + json aggregates over `routes`+`param_schema`) floors at ~1k-1.2k req/s on dynamic routes. Controls prove the engine itself sustains 34k+ on a point query (`/q2`: one table, `SELECT to_json... WHERE id=1`) and transport ~38k (`/ping` zero-DB). The tax is "OLAP query shape per 1-row request".

The crossing: keep `routes`/`param_schema` (and Tier-1 `handle_request` macro) as the honest registry, move *only* the structural work (segment split/match/most-literal tiebreak + extract + try_cast validation + {param}->literal render) into C. Hand the *rendered handler SQL only* (or a prebuilt body) to DuckDB. Static/openapi/html served from C memory with zero DB touch on hot path. The C layer is shared by any compiled extension flavor (capi or C++).

This report designs that shared layer concretely. All behaviors must be byte-identical to the macro for 404/422 bodies, handler_sql rendering, FastAPI-shaped detail[], etc.

---

## 1. Boot: Load routes + param_schema into C structs ONCE

At extension init or `serve_xxx(port, db_path)` entry (before accept_loop + worker pool):

- `duckdb_open(db_path, &db)` (or receive an existing handle from extension context).
- One worker-style connect.
- `duckdb_query` (or prepared+fetch) two registry queries. Iterate results with `duckdb_value_*`, `duckdb_row_count`.
- Build immutable structs. Free result. (Optionally keep one conn for boot-time pre-renders.)
- Load extensions/settings on conns happens in workers as today.
- Routes are append-only config; assume loaded before threads start. No live mutation in v1.

### Structs (pure C, malloc-owned, no C++ std)

```c
/* constraint support: only le/ge for int today (per framework); extensible */
typedef struct {
    bool has_le;
    long long le;
    bool has_ge;
    long long ge;
    /* future: enum json, min_length etc. parse once at load */
} ParamConstraint;

typedef struct {
    char   *name;           /* owned */
    char   *location;       /* "path" | "query" | "body" */
    char   *type;           /* "int" | "float" | "bool" | "string" */
    bool    required;
    char   *constraint_json;/* raw or NULL; parsed into c on load */
    ParamConstraint c;
} ParamDef;

typedef struct {
    char   *route_id;
    char   *method;         /* upper, e.g. "GET" */
    char   *pattern;        /* original "/users/{id}" */
    char   *handler;        /* template with {name} */
    char   *kind;           /* "dynamic" | "stream" | "static" | "openapi" | "html" */
    int     status;

    /* pre-split (exact match to SQL list_filter(split, len>0)) */
    char  **pat_segs;       /* owned strings, count == seg_count */
    int     seg_count;
    int     literal_count;  /* # of pat_segs that do NOT start with '{' */

    ParamDef *params;       /* owned array for this route only */
    int       param_count;

    /* pre-rendered bodies for zero-DB statics (populated at boot) */
    char   *static_body;    /* for static/html/openapi; NULL for dynamic */
    char   *static_ct;      /* "application/json" or "text/html" */
    bool    is_stream;      /* kind=="stream" */
} RouteDef;

typedef struct {
    RouteDef *routes;
    int       count;
    /* fast-path optionals (not required for correctness) */
    /* e.g. a tiny exact-match table for 0-param routes, or hash by "METHOD /path" */
} RouteTable;
```

**Load sketch (error paths elided):**

```c
RouteTable* quack_load_registry(void *ddb_con) {
    RouteTable *rt = calloc(1, sizeof(*rt));
    /* query routes */
    /* SELECT route_id, method, pattern, handler, kind, status FROM routes */
    ddb_result r; duckdb_query(ddb_con, "...", &r);
    rt->count = duckdb_row_count(&r);
    rt->routes = calloc(rt->count, sizeof(RouteDef));
    for (int i=0; i<rt->count; i++) {
        RouteDef *rd = &rt->routes[i];
        rd->route_id = strdup(duckdb_value_varchar(&r, 0, i));
        ... same for others ...
        rd->status = duckdb_value_int32(&r, 5, i);
        quack_split_segments(rd->pattern, &rd->pat_segs, &rd->seg_count);
        rd->literal_count = 0;
        for (int j=0; j<rd->seg_count; j++)
            if (rd->pat_segs[j][0] != '{') rd->literal_count++;
    }
    duckdb_destroy_result(&r);

    /* query params, group by route_id (small N: O(n*m) scan or sort) */
    /* SELECT route_id, name, location, type, required, constraint_json FROM param_schema */
    ... for each row, find owning RouteDef by route_id strcmp, realloc+append ParamDef ...
    for each ParamDef: parse_constraint(pd->constraint_json, &pd->c);

    /* pre-render static bodies (purely from loaded data or one boot query) */
    for each rd {
        if (strcmp(rd->kind, "static")==0) {
            rd->static_body = strdup(rd->handler);
            rd->static_ct   = strdup("application/json");
        } else if (strcmp(rd->kind, "html")==0) {
            rd->static_body = strdup(rd->handler); /* the long <!DOCTYPE... */
            rd->static_ct   = strdup("text/html");
        } else if (strcmp(rd->kind, "openapi")==0) {
            rd->static_body = quack_build_openapi_json(rt); /* C impl over rt, or ONE boot query */
            rd->static_ct   = strdup("application/json");
        }
        rd->is_stream = (strcmp(rd->kind,"stream")==0);
    }
    return rt;
}
```

`quack_split_segments(const char *p, char ***out, int *n)`: manual split on '/', skip empty segments (leading `/`, `//`, trailing). Exact replica of SQL `list_filter(string_split(...,'/'), lambda x: len(x)>0)`.

`parse_constraint`: minimal strstr + strtoll on `"le":123` / `"ge":...` (only ints today). NULL json => no constraints.

---

## 2. Per-request in C (zero SQL until handler)

Called from worker `handle_conn_on` (after raw HTTP parse of method/path/body/headers_json):

```c
typedef struct {
    int   status;
    char *content_type;   /* owned or static */
    char *body;           /* owned or NULL (dynamic/stream) or static */
    char *handler_sql;    /* owned or NULL */
    bool  is_stream;
} RouteDecision;
```

`RouteDecision quack_route(const RouteTable *rt, const char *method, const char *path, const char *body);`

**Inside ( ~15 lines for the match core):**

1. Split path (replicate `path_query` + `req`):
   - Find first '?'; clean_path = prefix, query_str = suffix or "".
   - `quack_split_segments(clean_path, &req_segs, &n_req);`  (stack arrays or small malloc; max depth ~16).
   - Parse query: split '&' -> key=val (no %decode, match SQL exactly). Produce a tiny `QParam {char *k,*v;} qparams[16]; int nq;`. Or linear scan later.

2. Most-literal match (exact replica of `route_idx` + `matched` + QUALIFY):
   ```c
   int best = -1, best_lit = -1;
   char best_rid[256] = {0};
   for (int i=0; i<rt->count; i++) {
       const RouteDef *rd = &rt->routes[i];
       if (strcmp(rd->method, method) != 0) continue;
       if (rd->seg_count != n_req) continue;
       bool m = true;
       for (int j=0; j<n_req; j++) {
           const char *pat = rd->pat_segs[j];
           if (pat[0] == '{') continue;
           if (strcmp(pat, req_segs[j]) != 0) { m=false; break; }
       }
       if (!m) continue;
       if (rd->literal_count > best_lit ||
           (rd->literal_count == best_lit && strcmp(rd->route_id, best_rid) < 0)) {
           best = i; best_lit = rd->literal_count; strncpy(best_rid, rd->route_id, sizeof(best_rid)-1);
       }
   }
   ```
   This is the core ~15-line C path. No window, no lambdas, no list_zip per call.

3. If best<0: 404 decision. body=`{"detail":"Not Found"}`, handler_sql=NULL.

4. Extract path pmap + query + body vals (for the best route's ParamDef list):
   - Path map: for pats starting '{', name=substr(1,-1), val=req_segs[j]. Small array or linear.
   - Query: linear lookup in qparams (or build tiny map).
   - Body: for "body" params, `json_extract_string_c(body, name)` (see below). NULL/empty body => NULL val.

5. Validation (replicate validation_errors + err_agg):
   - For each ParamDef of best:
     - val_str from above.
     - if (required && !val_str) "missing"
     - if (type=="int" && val_str && !valid_int(val_str)) "int_parsing"  (strtoll + endptr)
     - float: strtod + endptr
     - bool: match what try_cast accepts (lower(trim) in {"true","false","1","0"} etc.; produce "bool_parsing")
     - if int && val && constraint: parsed = atoll; if (has_le && parsed > le) "less_than_equal" etc.
   - Collect error structs `{name,location,type,err_code,constraint_json}`.
   - If any: status=422, content_type=json,
     body = `{"detail":[ {"type":..., "loc":[location,name], "msg": exact FastAPI phrasing} , ... ] }`
     (build with snprintf or a small json array emitter; match messages exactly including the "less_than_equal to X" text).
     handler_sql=NULL.

6. No errors:
   - If kind static/openapi/html (or static_body != NULL): decision with prebuilt static_body, static_ct, status=rd->status, handler_sql=NULL.
   - Else (dynamic or stream):
     - Build param literals array (same rules as `param_literals`):
       - int/float: val_str as-is (post-validation)
       - bool: lower(val_str)
       - string (or null): `'` + replace(' with '') + `'`   or `NULL`
     - Render: start with strdup(handler). For each param in route order: `replace_all( h, "{"+name+"}", literal )`. (Exact substring of full `{name}`; replicate list_reduce.)
     - decision.handler_sql = h; body=NULL; ct = is_stream ? "text/event-stream" : "application/json"; status=rd->status.

Return decision (caller frees the three char* if non-static).

**Minimal helpers required (no external deps):**
- `char *json_extract_string_c(const char *json, const char *key);` — flat top-level object only. Handles `"k": "str"`, `"k":123`, `"k":true`, missing->NULL. Returns malloc'd unquoted string or number-as-text. Replicates DuckDB json_extract_string semantics for the schema cases used.
- `char *str_replace(const char *src, const char *sub, const char *rep);` or in-place builder for the reduce step.
- `bool valid_int(const char *s); long long parse_int...` etc. Trim optional.
- URL query split + path split exactly as SQL.

---

## 3. Hand-off to DuckDB (or pure C static serve)

In the worker (C, after router decision):

```c
if (dec.handler_sql && dec.handler_sql[0]) {
    if (dec.is_stream) { /* chunked write loop as today, using duckdb_query on hsql */ }
    else {
        ddb_result hr;
        duckdb_query(con, dec.handler_sql, &hr);  /* or prepare+execute_prepared if preferred */
        ... extract row 0 col 0 as final body ...
    }
} else if (dec.body) {
    /* serve directly: write headers + dec.body. ZERO duckdb_* calls. */
}
```

Exactly as current diagnostics `/ping` and static paths, and as `rendered_static` bodies.

For Tier-1 pure-SQL callers: `handle_request` macro unchanged (ergonomic surface).

---

## 4. Throughput ceiling + sharp edges

**Estimate (grounded in existing controls):**

- Static /health /docs /openapi (pre-rendered): 30k–38k req/s. Near `/ping` (zero DB) and the "materialized 28k" mirage (but now honest in-memory). C match cost is noise (<10µs). Bottleneck = socket read/parse/write + ab/curl client.
- Dynamic routes whose handler is a simple point query or small read (e.g. `/users/1`, `/users`, `/search` with small result): 25k–34k req/s. Approach `/q2` 34k floor. Delta = C path split+match+validate+render (~5-30µs) + per-request prepare of the *literalized* handler SQL + one row exec. Still 15-30× the old brain.
- Complex handlers (joins, shellfs, large results, writes with self-dispatch): limited by the handler itself, not router (same as today).
- With 16 workers + keepalive + TCP_NODELAY (already in): scales to the per-worker query floor. No more "big CTE rebind under catalog lock".

**Sharp edges (named, concrete):**

- **Per-request allocation**: path/query split produces small arrays (use `char *segs[32];` stack + index or strdup only values). Rendered hsql: malloc(template_len + overhead for quotes). Error JSON and 404 body: small mallocs. At 30k rps × 16 workers this is real pressure. Mitigations: per-worker fixed-size arena (reset per request), stack buffers + bounded paths, reuse decision buffers. Measure with Instruments/malloc.
- **Prepared-statement reuse / caching by handler**: Rendered hsql contains *literal values* (e.g. `id = 123`, `q = 'al'`). Distinct per value combination → cannot share a prepared stmt across different path/query values. High-cardinality params (ids, search terms) thrash any naive `map<sql_text, stmt>` cache. Low-cardinality routes benefit. Current `duckdb_query` (or per-req prepare+exec+destroy) is what /q2 already pays; we inherit it. Future escape hatch (not in this design): optional "bindable handler" form with `?` + value vector, dual execution path.
- **Param binding**: Not used for the final handler (by design — literal render produces standalone SQL for Tier-1 self-dispatch and dispatch.sql). C "binds" by doing the try_cast + literalization. The 4-arg handle_request prepared stmt (current) goes away for Tier-2 hot path.
- **Thread-safety of shared routes table**: Read-only after `quack_load_registry`. All workers read concurrently — safe with no locks (plain loads). If future "INSERT INTO routes while serving" support is added, need: mutex around match, or RCU (swap ptr to new RouteTable after rebuild), or reload-on-demand with generation counter. v1: "load once at startup" contract.
- **SQL-injection safety of literal templating**: 
  - Numbers/bools: post-validation numeric strings only; inlined unquoted.
  - Strings: outer `'`, inner `'` → `''` (exact match to SQL). This is the standard defense for value literals.
  - Names come from trusted `param_schema` (registry rows), not wire.
  - Substitutions use full `"{name}"` token (not bare name).
  - Still: if a handler author puts a `{param}` into an identifier position (`FROM {tbl}`), or if validation is buggy allowing `1; DROP`, injection surface exists — but same as the pure-SQL version today. C must not add new escapes or forget the double-single-quote.
  - NUL bytes or non-UTF8 in values: current SQL path also passes them; C strings must use length-aware or ensure DuckDB accepts.
- **Where C-routing *still must* call the DB**:
  - Any `dynamic`/`stream` route: execute the (now trivial) rendered handler_sql to produce the actual response body/rows. This is the desired 34k path.
  - Handlers that read/write app tables, call `read_text`/`shellfs`, or self-dispatch.
  - Worker connection setup (SET threads=1, LOAD curl_httpfs + settings — done once per worker).
  - Boot: the two registry SELECTs + (if chosen) one openapi pre-render SELECT or equivalent.
  - /openapi.json after boot: served from prebuilt C string; no per-req call.
  - Error paths and 404 never touch DB.
  - Note: even "static" handlers that happen to be constant SQL still go through render+exec unless marked static at registration.

Other edges: query-string has no %-decoding (same as SQL today — document); duplicate query keys (last wins or map semantics); very long paths/bodies (current 2k/64k buffers); case sensitivity of methods (upper in registry).

---

## 5. C API vs C++ API

The design uses only:
- DuckDB *client* C API (`duckdb_open`, `connect`, `query`/`prepare`/`bind_varchar`/`execute_prepared`, `row_count`, `value_varchar`/`int32`, `free`, `destroy_*`, result error, streaming hooks).
- pthreads, sockets, str* (already proven in ducktinycc + serve_brain).
- Manual memory (calloc/strdup/free) + small fixed buffers.
- For the UDF surface (`serve_brain` entry): the **C Extension API** (experimental template + reference-extension-c + capi_quack) which exposes a stable struct of function pointers (`duckdb_create_scalar_function`, `duckdb_register_scalar_function`, etc.) usable from pure C. No dependency on the internal/unstable C++ API.

**Nothing in this layer requires C++ DuckDB internals** (the volatile `duckdb::Connection::CreateScalarFunction` template sugar, DataChunk vectorization beyond what's needed, etc.). The router is ordinary C string/loop work. OpenAPI pre-render is either C string building or a one-time client query. Handler execution uses the same C client symbols already dlsym'd today.

C++ would be *nicer* (std::string, std::vector<RouteDef>, std::map for tiny lookups, unique_ptr, easier RAII for results) and is the "normal" way most extensions are written, but it is not *forced*. The design is achievable with the C Extension API + C client API alone. (If a future need arises for native vectorized table functions returning Arrow or custom scan replacement for the registry, that would tilt C++ — but not for routing+validation+literal render.)

---

## VERDICT

**Expected req/s ceiling for routing-in-C: 25k–34k+ for dynamic routes (approaching the proven `/q2` 34k floor once the 13-op OLAP brain is removed from the per-request path); 30k–38k for static/openapi/html (near pure-transport `/ping`).** The router match itself disappears into noise; the remaining cost is exactly the handler execution (or zero for statics) plus unavoidable socket/HTTP framing.

**The design is achievable with the C Extension API alone: YES.**

The Tier-1 pure-SQL surface (`handle_request` macro + `routes`/`param_schema` as config-as-data) stays untouched and correct. The C layer is the Tier-2 compiled hot path that finally lets quackapi cross the "one OLAP query per request" wall without materializing lies.

