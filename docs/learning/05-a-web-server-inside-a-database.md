# 05 — A Web Server Inside a Database

This is the guided tour of `quackapi_brain.cpp`. The file implements a complete HTTP/1.1 server (listen/accept/worker pool) that lives inside the DuckDB process and calls back into DuckDB (via the C API symbols) to execute handlers.

Everything here was first written as pure SQL + a tiny C fragment JIT'd by tcc, then ported almost verbatim into the compiled extension so the hot path could escape the cost of the `handle_request` macro.

## Socket / bind / listen / accept (the accept loop)

```cpp
// ext-cpp/src/quackapi_brain.cpp:500
const char *serve_brain_impl(int port, const char *db_path){
	if(g_listen < 0){
		signal(SIGPIPE, SIG_IGN);
		g_listen = socket(2, 1, 0);
		... setsockopt, bind, listen(g_listen, 128) ...
		... resolve all the duckdb_* symbols via dlsym ...
		g_ddb_open(db_path, &g_db);
		...
	}
	... start worker pool once ...
	pthread_create(&srv, nullptr, accept_loop, nullptr);
	...
}
```

```cpp
// 489
void *accept_loop(void *arg){
	for(;;){
		int c = accept(g_listen, 0, 0); if(c < 0) continue;
		{ int nd = 1; setsockopt(c, 6, 1, &nd, 4); } /* TCP_NODELAY */
		... enqueue to g_q under mutex, signal cond ...
	}
}
```

Classic single-listener, N-worker model. The listener thread only accepts and enqueues file descriptors. All DuckDB work happens on the 16 detached worker threads.

`SIGPIPE` is ignored because a client can close the socket between the time you accept and the time you write the response; you want `write` to return EPIPE, not kill the process.

## The pthread worker pool + persistent connections

```cpp
// 490
static void *worker_main(void *arg){
	void *con = 0;
	void *stmt = 0;
	if(g_ddb_connect(g_db, &con) != 0) return 0;

	{ g_ddb_query(con, "SET threads=1", &ign); ... }
	{ ... LOAD shellfs, curl_httpfs, httpfs_timeout_retry ... }
	{ SET httpfs_client_implementation='curl'; ... }

	for(;;){
		pthread_mutex_lock(&g_qm);
		while(g_qcount == 0) pthread_cond_wait(&g_qcv, &g_qm);
		int fd = g_q[g_qhead]; ... g_qcount--;
		pthread_mutex_unlock(&g_qm);
		handle_conn_on(con, fd);
	}
}
```

Key points:

- 16 workers (`NWORKERS`).
- Each worker opens **one persistent** connection at startup and reuses it for every request it handles.
- `SET threads=1` is issued per worker. DuckDB's default is to use as many threads as cores for *each* query. With 16 concurrent queries each wanting the full core count, you get massive scheduler thrash. `threads=1` makes each individual query single-threaded; the 16 connections supply the concurrency.
- The HTTP client policy (`curl` vs `httplib`) is set here and must be mirrored in `framework.sql` (the "double-wired" comment at framework.sql:20).

## g_rt — the per-process static registry

```cpp
// brain.cpp:200
static RouteTable *g_rt = NULL;
```

`RouteTable` (defined around line 99) is a hand-written struct containing arrays of `RouteDef`, each with pre-split segments, literal counts, `ParamDef` arrays, and pre-rendered static bodies.

It is populated by `quack_load_registry` (called from `serve_brain_impl` and from `quack_init_router` / `quack_reload_router`).

After boot it is **read-only** for the hot path. `quack_route` never takes a lock while walking it.

This is why `CREATE ROUTE` must call `quack_reload_router` after writing the tables: the in-memory compiled form must be rebuilt from the SSOT tables.

Cross-process: another process mapping the same .db file has its own `g_rt` in its own address space. See document 03.

## The fast-path diagnostics (why they exist)

Before the real router:

```cpp
// 215
if(strcmp(path, "/ping") == 0){
	char pong[] = "..."; write(...); close(fd); return;
}
if(strcmp(path, "/q1") == 0){ g_ddb_query(con, "SELECT 42", ...); ... }
if(strcmp(path, "/q2") == 0){ ... point query on users ... }
if(strcmp(path, "/q3") == 0){ ... count(*) from routes ... }
```

These were used during B1/B2 measurement to separate:
- pure socket accept/write throughput,
- per-statement DuckDB floor (parse+plan+exec of trivial query),
- cost of touching one catalog table,
- full macro cost.

`/ping` in particular lets you measure whether the C layer or the query layer is the current ceiling.

## handle_conn_on — the request path

After the fast-paths, the code:
1. Parses the request line (method, path) with manual pointer walking.
2. Parses body after `\r\n\r\n`.
3. Builds a small headers JSON (lowercased keys, escaped).
4. If `g_rt`, calls `quack_route(g_rt, method, path, body)`.
5. Based on the `RouteDecision`, either:
   - Serves a static body with zero DB calls.
   - Executes `g_ddb_query(con, dec.handler_sql, ...)` and streams or returns the first column value.
   - Returns 404/422/500 JSON.

For streams (`kind='stream'`) it uses chunked transfer encoding and `Transfer-Encoding: chunked`.

## quack_route — line-by-line mirror of framework.sql

The function `quack_route` (brain:902) is the compiled version of the `handle_request` macro's logic.

Compare side by side:

**Splitting**

framework.sql:118 (path_query + req):
```sql
list_filter(string_split(clean_path, '/'), lambda x: len(x) > 0) AS req_segs
```

brain:920:
```cpp
quack_split_segments(clean, &req_segs, &nreq);
```

**Route index + literal pre-count**

framework:149 (route_idx):
```sql
len(list_filter(..., lambda s: NOT starts_with(s, '{'))) AS literal_count
```

brain:805 (during load):
```cpp
rd->literal_count = 0;
for (int j = 0; j < rd->seg_count; j++) {
	if (rd->pat_segs[j][0] != '{') rd->literal_count++;
}
```

**Match + most-literal tiebreak**

framework:160 (matched):
```sql
...
QUALIFY row_number() OVER (ORDER BY ri.literal_count DESC, ri.route_id) = 1
```

brain:929:
```cpp
for (int i = 0; i < rt->count; i++) {
	...
	if (rd->literal_count > best_lit ||
	    (rd->literal_count == best_lit && (best < 0 || strcmp(...) < 0))) {
		best = i; best_lit = rd->literal_count; ...
	}
}
```

Exact same rule: more literals wins; on tie, smaller route_id wins (stable).

**Param extraction + validation**

framework:210 uses `try_cast(... AS BIGINT) IS NULL` → 'int_parsing', and similar for float/bool, plus constraint checks on `constraint_json`.

brain:985 (`quack_parse_int`, `quack_is_valid_bool`, constraint check) produces identical error codes: `int_parsing`, `float_parsing`, `bool_parsing`, `missing`, `less_than_equal`, `greater_than_equal`.

**422 shape**

framework:250 builds `json_group_array( json_object('type', ..., 'loc', json_array(...), 'msg', ...) )`

brain:1020 builds the identical array of objects with the same messages (including the dynamic "less than or equal to N" text pulled from the live constraint).

**Templating**

framework:290 (`handler_rendered`) uses `list_reduce( list_transform(..., replace ...`

brain:1100:
```cpp
char *rpl = quack_str_replace(hsql, tok, lit);
...
if (val[0] == 0) {
	strcpy(lit, "NULL");
} else if (int or float) { copy raw } else if (bool) { lower } else {
	... quote + double '' for inner quotes ...
}
```

String literals are escaped exactly the way SQL requires (`'` → `''`).

## Statics served with zero DB calls

During `quack_load_registry`:

```cpp
// brain:869
for (int ri = 0; ri < rt->count; ri++) {
	RouteDef *rd = &rt->routes[ri];
	if (strcmp(rd->kind, "dynamic") == 0 || strcmp(rd->kind, "stream") == 0) continue;
	char q[512];
	snprintf(q, sizeof(q), "SELECT body, content_type FROM handle_request('%s','%s','{}','')", ...);
	... run the query once at boot, strdup the body and ct into rd->static_body / static_ct ...
}
```

Later in `handle_conn_on`, if `dec.body` (from static) is present, it is written directly with no `g_ddb_query` at all.

This is the source of the 39k–44k req/s numbers for `/health` in B2_RESULT.md.

## Where the 1k → 30k speedup actually comes from

B1 baseline (pure macro path) was ~1.3–1.4k req/s for a simple dynamic route.

B2 (C router):

- Static paths: 39k–44k (the macro + its 13-operator plan is never entered).
- Simple dynamic list: 27k–31k.
- Param + handler: 26k–35k.
- Query-param search: 15k–19k (still far above B1).

The work that left the hot path per request:

1. The entire `handle_request` CTE tree (segmentation, route_idx materialization, QUALIFY window, param_values join, validation_errors CTEs, handler_rendered reduce, etc.).
2. For statics: even the *call* into the macro and the read of `routes`/`param_schema`.
3. Per-request JSON construction for headers in some paths (still done, but now in tight C rather than SQL string building).

What remains on the hot path for a typical dynamic route:
- C string split + linear match + literal tiebreak (very small).
- Validation loops over the (usually tiny) param list.
- One `duckdb_query` of the *already rendered* handler SQL (the thing the user wrote in `AS ...`).
- Write of the result column value.

The expensive part (planning the big router query every time) is gone. The user's handler still pays its normal parse/plan/exec cost, but that cost is now the only query cost.

## Keep-alive and the ab numbers

All the B2 numbers were taken with `ab -n 8000 -k` (keep-alive). Without `-k` you pay TCP + DuckDB connection setup per request even with the fast router. The numbers in B2_RESULT.md are therefore "what the C layer + one trivial query can sustain when connection costs are amortized."

## The hand-rolled router is a hack (be honest)

The segment matcher, the manual JSON escaping, the `quack_str_replace` for templating, the hand-built 422 array — all of this duplicates logic that a real web framework would express once in a routing table + a proper template or prepared statement emitter.

Production version would likely:
- Use a real path template library or at least compile the patterns once into an automaton.
- Use DuckDB's prepared statement API properly (bind parameters) instead of string substitution into `handler` for the final execution (the current design renders to SQL text so the worker can just `g_ddb_query(con, rendered, ...)` with the low-level symbols it already has).
- Still keep the static pre-render trick.

The current code is the minimal port that achieved parity and the measured speedup. It is intentionally low-level C string code inside a database process.

## Read it yourself (guided tour order)

1. `ext-cpp/src/quackapi_brain.cpp:500` — `serve_brain_impl` (socket + symbol resolution + first load of g_rt).
2. `ext-cpp/src/quackapi_brain.cpp:489` — `accept_loop`.
3. `ext-cpp/src/quackapi_brain.cpp:490` — `worker_main` (the SETs, the cond-wait loop, call to handle_conn_on).
4. `ext-cpp/src/quackapi_brain.cpp:210` — `handle_conn_on` start through the `/ping` `/q*` fast paths and header parsing.
5. `ext-cpp/src/quackapi_brain.cpp:360` — the `if (g_rt)` block and the three outcomes (handler_sql, static body, error).
6. `ext-cpp/src/quackapi_brain.cpp:902` — `quack_route` (the whole matching + validation + templating function).
7. `ext-cpp/src/quackapi_brain.cpp:780` — `quack_load_registry` (the pre-render of statics at 869 is the key part).
8. `ext-cpp/src/quackapi_brain.cpp:1130` — `quack_init_router` and `quack_reload_router`.
9. `framework.sql:149` — `route_idx` CTE (compare literal_count computation).
10. `framework.sql:160` — `matched` CTE + QUALIFY (compare the tiebreak).
11. `B2_RESULT.md:40` — the actual numbers and what was measured.

## Comprehension questions

1. In `worker_main`, why is `SET threads=1` issued on the worker connection rather than globally on the `g_db` handle? What would happen to concurrent request handling if it were a global setting changed only once?
2. Walk a request for `GET /health` from accept to write. List every place DuckDB (via g_ddb_*) is called. Now do the same walk for `GET /users/1`. Where does the difference in cost come from?
3. `quack_route` returns a `RouteDecision` containing either `body` (for static/404/422) or `handler_sql` (for dynamic). Who executes the `handler_sql`? On which connection? At what point in the C call stack?
4. The pre-render of static bodies happens with `handle_request(...)` at registry load time. Why is it safe to call the SQL macro from inside `quack_load_registry` (which is called from a worker connection at boot) but it would have been unsafe to call `context.Query` from `RouteDdlPlan`?
5. If you removed the `if (rd->static_body)` special case and always went through `g_ddb_query` even for `/health`, what would the new req/s number for that path be closest to, according to the B2 measurements?
