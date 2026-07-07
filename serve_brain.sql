INSTALL ducktinycc FROM community; LOAD ducktinycc;

-- ============================================================================
-- quackapi — serve_brain: threaded C accept loop + per-request call into the
-- DuckDB C API (via dlsym(RTLD_DEFAULT)) to invoke the PURE SQL handle_request
-- TABLE macro. The DB is the file-backed instance passed in: serve_brain(port, db_path).
-- No routing in C. Full path+query passed. Uses prepared stmt for safety.
-- ============================================================================

SELECT ok, code FROM tcc_module(mode := 'quick_compile',
  source := 'struct in_addr { unsigned int s_addr; };
struct sockaddr_in { unsigned char sin_len; unsigned char sin_family; unsigned short sin_port; struct in_addr sin_addr; char sin_zero[8]; };
int socket(int,int,int); int setsockopt(int,int,int,const void*,unsigned int);
int bind(int,const struct sockaddr_in*,unsigned int); int listen(int,int);
int accept(int,void*,void*); long read(int,void*,unsigned long);
long write(int,const void*,unsigned long); int close(int);
int snprintf(char*,unsigned long,const char*,...); int usleep(unsigned int);
char *strstr(const char*, const char*);
int strcmp(const char*, const char*);
unsigned long strlen(const char*);
void *memcpy(void*, const void*, unsigned long);
typedef void* pthread_t; int pthread_create(pthread_t*,void*,void*(*)(void*),void*); int pthread_detach(pthread_t);
typedef struct { char _pad[64]; } pmutex;
typedef struct { char _pad[48]; } pcond;
int pthread_mutex_init(pmutex*, void*);
int pthread_mutex_lock(pmutex*);
int pthread_mutex_unlock(pmutex*);
int pthread_cond_init(pcond*, void*);
int pthread_cond_wait(pcond*, pmutex*);
int pthread_cond_signal(pcond*);
int pthread_cond_broadcast(pcond*);
int pthread_join(pthread_t, void**);
void *dlsym(void*, const char*);
void *dlopen(const char*, int);
void *signal(int, void*);

typedef struct { char _pad[256]; } ddb_result;
typedef int (*ddb_open_t)(const char*, void**);
typedef int (*ddb_connect_t)(void*, void**);
typedef void (*ddb_disconnect_t)(void**);
typedef void (*ddb_close_t)(void**);
typedef int (*ddb_prepare_t)(void*, const char*, void**);
typedef int (*ddb_bind_varchar_t)(void*, unsigned long long, const char*);
typedef int (*ddb_execute_prepared_t)(void*, void*);
typedef void (*ddb_destroy_prepare_t)(void**);
typedef void (*ddb_destroy_result_t)(void*);
typedef unsigned long long (*ddb_row_count_t)(void*);
typedef int (*ddb_value_int32_t)(void*, unsigned long long, unsigned long long);
typedef char* (*ddb_value_varchar_t)(void*, unsigned long long, unsigned long long);
typedef void (*ddb_free_t)(void*);
typedef int (*ddb_query_t)(void*, const char*, void*);
typedef const char* (*ddb_result_error_t)(void*);

/* streaming result symbols (resolved if present) */
typedef int (*ddb_execute_prepared_streaming_t)(void*, void*);
typedef void* (*ddb_fetch_chunk_t)(void*);
typedef void (*ddb_destroy_data_chunk_t)(void**);
typedef unsigned long long (*ddb_chunk_get_size_t)(void*);
typedef void* (*ddb_chunk_get_vector_t)(void*, unsigned long long);
typedef void* (*ddb_vector_get_data_t)(void*);
typedef unsigned long long* (*ddb_vector_get_validity_t)(void*);

void* resolve_sym(const char* name) {
  void* h = dlsym((void*)-2, name);
  if (h) return h;
  void* m = dlopen(0, 2);
  if (m) {
    h = dlsym(m, name);
    if (h) return h;
  }
  m = dlopen("libduckdb.dylib", 2);
  if (m) {
    h = dlsym(m, name);
    if (h) return h;
  }
  m = dlopen("/opt/homebrew/opt/duckdb/lib/libduckdb.dylib", 2);
  if (m) {
    h = dlsym(m, name);
    if (h) return h;
  }
  m = dlopen("/opt/homebrew/lib/libduckdb.dylib", 2);
  if (m) {
    h = dlsym(m, name);
    if (h) return h;
  }
  /* Linux / split-lib fallbacks. On most setups RTLD_DEFAULT above already
     resolves every duckdb_* symbol from the running duckdb process, so these
     only matter when serving against a separately-installed libduckdb. */
  m = dlopen("libduckdb.so", 2);
  if (m) {
    h = dlsym(m, name);
    if (h) return h;
  }
  m = dlopen("/usr/local/lib/libduckdb.dylib", 2);
  if (m) {
    h = dlsym(m, name);
    if (h) return h;
  }
  m = dlopen("/usr/local/lib/libduckdb.so", 2);
  if (m) {
    h = dlsym(m, name);
    if (h) return h;
  }
  return 0;
}

static int g_listen = -1;
static void* g_db = 0;
static ddb_open_t g_ddb_open = 0;
static ddb_connect_t g_ddb_connect = 0;
static ddb_disconnect_t g_ddb_disconnect = 0;
static ddb_close_t g_ddb_close = 0;
static ddb_prepare_t g_ddb_prepare = 0;
static ddb_bind_varchar_t g_ddb_bind_varchar = 0;
static ddb_execute_prepared_t g_ddb_execute_prepared = 0;
static ddb_destroy_prepare_t g_ddb_destroy_prepare = 0;
static ddb_destroy_result_t g_ddb_destroy_result = 0;
static ddb_row_count_t g_ddb_row_count = 0;
static ddb_value_int32_t g_ddb_value_int32 = 0;
static ddb_value_varchar_t g_ddb_value_varchar = 0;
static ddb_free_t g_ddb_free = 0;
static ddb_query_t g_ddb_query = 0;
static ddb_result_error_t g_ddb_result_error = 0;
static ddb_execute_prepared_streaming_t g_ddb_execute_prepared_streaming = 0;
static ddb_fetch_chunk_t g_ddb_fetch_chunk = 0;
static ddb_destroy_data_chunk_t g_ddb_destroy_data_chunk = 0;
static ddb_chunk_get_size_t g_ddb_chunk_get_size = 0;
static ddb_chunk_get_vector_t g_ddb_chunk_get_vector = 0;
static ddb_vector_get_data_t g_ddb_vector_get_data = 0;
static ddb_vector_get_validity_t g_ddb_vector_get_validity = 0;

#define NWORKERS 16
static int g_q[4096]; static int g_qhead=0, g_qtail=0, g_qcount=0;
static pmutex g_qm; static pcond g_qcv;
static int g_pool_started = 0;
void handle_conn_on(void *con, void *stmt, int fd){
  char req[65536];
  char method[16];
  char path[2048];
  char body[65536];
  long n = read(fd, req, 65535); if(n < 0) n = 0; req[n] = 0;
  int i = 0, j = 0;
  while(req[i] && req[i] != 32 && i < 15){ method[i] = req[i]; i++; }
  method[i] = 0;
  if(req[i] == 32) i++;
  j = 0;
  while(req[i] && req[i] != 32 && j < 2047){ path[j++] = req[i++]; }
  path[j] = 0;
  /* DIAGNOSTIC fast-path: /ping returns a fixed reply with ZERO DuckDB calls.
     Isolates pure socket-layer throughput from per-query engine overhead so we
     can tell whether the ceiling is the C layer or the query layer. */
  if(strcmp(path, "/ping") == 0){
    char pong[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 4\r\nConnection: close\r\n\r\npong";
    write(fd, pong, sizeof(pong)-1); close(fd); return;
  }
  /* DIAGNOSTIC: /q1 runs a TRIVIAL query (no macro, no params) on the worker
     connection. Splits DuckDB per-statement floor (parse+plan+exec of SELECT 42)
     from the fat handle_request macro per-execute bind/optimize cost. */
  if(strcmp(path, "/q1") == 0){
    ddb_result rq; int rcq = g_ddb_query(con, "SELECT 42", &rq);
    if(rcq == 0) g_ddb_destroy_result(&rq);
    char q[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"q1\":true}";
    write(fd, q, sizeof(q)-1); close(fd); return;
  }
  /* DIAGNOSTIC: /q2 runs a SINGLE-table point query (one catalog object: users)
     via duckdb_query. Isolates "does touching ANY table serialize?" from the full
     brain (which binds route_index + param_schema + response_cache + users). */
  if(strcmp(path, "/q2") == 0){
    ddb_result rq; int rcq = g_ddb_query(con, "SELECT to_json(u) FROM users u WHERE u.id = 1", &rq);
    if(rcq == 0) g_ddb_destroy_result(&rq);
    char q[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"q2\":true}";
    write(fd, q, sizeof(q)-1); close(fd); return;
  }
  /* DIAGNOSTIC: /q3 touches one catalog object with the simplest possible read. */
  if(strcmp(path, "/q3") == 0){
    ddb_result rq; int rcq = g_ddb_query(con, "SELECT count(*) FROM routes", &rq);
    if(rcq == 0) g_ddb_destroy_result(&rq);
    char q[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"q3\":true}";
    write(fd, q, sizeof(q)-1); close(fd); return;
  }
  if(strstr(path, "slow")) usleep(300000); /* demo knob: any path containing "slow" naps 300ms to SIMULATE awaited handler IO */
  char *sep = strstr(req, "\r\n\r\n");
  if(sep){
    sep += 4;
    j = 0;
    while(sep[j] && j < 65535){ body[j] = sep[j]; j++; }
    body[j] = 0;
  } else {
    body[0] = 0;
  }
  /* parse headers into json; lower keys; escape " and \ in values; add _cookies */
  char headers_json[8192];
  int hpos = 0;
  headers_json[hpos++] = ''{'' ;
  int hfirst = 1;
  char cookie_val[4096]; cookie_val[0] = 0;
  char *hstart = strstr(req, "\r\n");
  if (hstart) hstart += 2;
  char *hend = sep ? sep : (req + n);
  char *hp = hstart ? hstart : req;
  while (hp && hp < hend) {
    char *eol = strstr(hp, "\r\n");
    if (!eol || eol > hend) eol = hend;
    if (eol == hp) break;
    char *colon = 0;
    char *cp = hp;
    while (cp < eol) { if (*cp == '':'' ) { colon = cp; break; } cp++; }
    if (colon) {
      char kbuf[256]; int ki = 0;
      char *ks = hp;
      while (ks < colon && ki < 255) {
        char ch = *ks;
        if (ch >= ''A'' && ch <= ''Z'') ch += 32;
        if (ch != '' '' && ch != ''\t'') kbuf[ki++] = ch;
        ks++;
      }
      kbuf[ki] = 0;
      char *vs = colon + 1;
      while (vs < eol && (*vs == '' '' || *vs == ''\t'')) vs++;
      char vbuf[4096]; int vi = 0;
      char *ve = eol;
      while (vs < ve && vi < 4095) vbuf[vi++] = *vs++;
      while (vi > 0 && (vbuf[vi-1] == '' '' || vbuf[vi-1] == ''\t'')) vi--;
      vbuf[vi] = 0;
      if (strcmp(kbuf, "cookie") == 0) {
        int cvi = 0; while (vbuf[cvi] && cvi < 4095) { cookie_val[cvi] = vbuf[cvi]; cvi++; } cookie_val[cvi] = 0;
      }
      if (!hfirst) headers_json[hpos++] = '','' ;
      hfirst = 0;
      headers_json[hpos++] = ''"'' ;
      int kii = 0; while (kbuf[kii] && hpos < 8190) headers_json[hpos++] = kbuf[kii++];
      headers_json[hpos++] = ''"'' ; headers_json[hpos++] = '':'' ;
      headers_json[hpos++] = ''"'' ;
      char *vp = vbuf;
      while (*vp && hpos < 8190) {
        if (*vp == ''"'' ) { headers_json[hpos++] = ''\\'' ; headers_json[hpos++] = ''"'' ; }
        else if (*vp == ''\\'' ) { headers_json[hpos++] = ''\\'' ; headers_json[hpos++] = ''\\'' ; }
        else headers_json[hpos++] = *vp;
        vp++;
      }
      headers_json[hpos++] = ''"'' ;
    }
    if (eol >= hend) break;
    hp = eol + 2;
  }
  if (cookie_val[0]) {
    if (!hfirst) headers_json[hpos++] = '','' ;
    const char *ckpre = "\"_cookies\":{";
    int cpi=0; while(ckpre[cpi] && hpos<8190){ headers_json[hpos++]=ckpre[cpi++]; }
    int cfirst = 1;
    char *cp = cookie_val;
    while (*cp) {
      while (*cp && (*cp=='';'' || *cp=='' '' || *cp==''\t'')) cp++;
      if (!*cp) break;
      char *eqp = 0; char *cep = cp;
      while (*cep && *cep != '';'') { if (!eqp && *cep==''='') eqp = cep; cep++; }
      char ckb[256]; int cki=0;
      char *ckpp = cp; char *ckend = eqp ? eqp : cep;
      while (ckpp < ckend && cki<255) {
        char ch=*ckpp; if(ch>=''A''&&ch<=''Z'') ch+=32; if(ch!='' ''&&ch!='' \t'') ckb[cki++]=ch; ckpp++;
      }
      ckb[cki]=0;
      char cvb[1024]; int cvi=0;
      if (eqp) {
        char *cvpp = eqp+1; while(cvpp<cep && (*cvpp=='' ''||*cvpp==''\t'')) cvpp++;
        char *cvend = cep; while(cvend>cvpp && (cvend[-1]=='' ''||cvend[-1]==''\t'')) cvend--;
        while(cvpp < cvend && cvi<1023) cvb[cvi++]=*cvpp++;
      }
      cvb[cvi]=0;
      if (!cfirst) headers_json[hpos++] = '','' ;
      cfirst = 0;
      headers_json[hpos++] = ''"'' ;
      int ckii=0; while(ckb[ckii]&&hpos<8190) headers_json[hpos++]=ckb[ckii++];
      headers_json[hpos++] = ''"'' ; headers_json[hpos++] = '':'' ;
      headers_json[hpos++] = ''"'' ;
      char *cvp = cvb; while(*cvp && hpos<8190){
        if(*cvp==''"''){ headers_json[hpos++]=''\\''; headers_json[hpos++]='' "''; }
        else if(*cvp==''\\''){ headers_json[hpos++]=''\\''; headers_json[hpos++]=''\\''; }
        else headers_json[hpos++] = *cvp;
        cvp++;
      }
      headers_json[hpos++] = ''"'' ;
      cp = *cep ? cep + 1 : cep;
    }
    headers_json[hpos++] = ''}'' ;
  }
  headers_json[hpos++] = ''}'' ;
  headers_json[hpos] = 0;
  if (hpos <= 2) { headers_json[0]='' {''; headers_json[1]='' }''; headers_json[2]=0; }
  /* con + stmt are PERSISTENT per worker (built once in worker_main): no connect / LOAD / prepare per request.
     PURE TRACK: stmt is the handle_request(method,path,headers,body) TABLE macro. Routing, validation,
     OpenAPI and handler templating all happen INSIDE that single SQL call over the routes/param_schema
     registry — no precomputed/materialized route tables, no fast-lane hash probe. The router IS the query. */
  ddb_result res;
  int rc = g_ddb_bind_varchar(stmt, 1, method);
  rc |= g_ddb_bind_varchar(stmt, 2, path);
  rc |= g_ddb_bind_varchar(stmt, 3, headers_json);
  rc |= g_ddb_bind_varchar(stmt, 4, body);
  if(rc != 0){
    char e[] = "HTTP/1.1 500 OK\r\nContent-Type: application/json\r\nContent-Length: 22\r\nConnection: close\r\n\r\n{\"error\":\"bind\"}";
    write(fd, e, sizeof(e)-1); close(fd); return;
  }
  rc = g_ddb_execute_prepared(stmt, &res);
  if(rc != 0){
    const char *errmsg = g_ddb_result_error ? g_ddb_result_error(&res) : 0;
    if(!errmsg) errmsg = "(no detail)";
    char ebody[4096];
    int ebl = snprintf(ebody, 4096, "{\"error\":\"execute\",\"detail\":\"%s\"}", errmsg);
    char eresp[4608];
    int erl = snprintf(eresp, 4608, "HTTP/1.1 500 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", ebl, ebody);
    write(fd, eresp, erl);
    g_ddb_destroy_result(&res); close(fd); return;
  }
  int status = g_ddb_value_int32(&res, 0ULL, 0ULL);
  char *ct = g_ddb_value_varchar(&res, 1ULL, 0ULL);
  char *bod = g_ddb_value_varchar(&res, 2ULL, 0ULL);
  char *hsql = g_ddb_value_varchar(&res, 3ULL, 0ULL);
  char *final_bod = bod;
  int final_status = status;
  const char *reason = "OK";
  if(status == 404) reason = "Not Found";
  else if(status == 422) reason = "Unprocessable Entity";
  const char *ctype = (ct && ct[0]) ? ct : "application/json";
  int is_stream = (strcmp(ctype, "text/event-stream") == 0);
  if (is_stream && hsql && hsql[0]) {
    char hdr[1024];
    int hl = snprintf(hdr, 1024, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n", status, reason, ctype);
    write(fd, hdr, hl);
    ddb_result res2;
    int rc2 = g_ddb_query(con, hsql, &res2);
    if (rc2 != 0) {
      char em[] = "data: {\"error\":\"stream_handler\"}\n\n";
      char lhex[32]; int lh = snprintf(lhex,32,"%x\r\n", (int)(sizeof(em)-1));
      write(fd, lhex, lh); write(fd, em, sizeof(em)-1); write(fd, "\r\n0\r\n\r\n", 7);
      if (ct) g_ddb_free(ct); if (bod) g_ddb_free(bod); if (hsql) g_ddb_free(hsql);
      g_ddb_destroy_result(&res); close(fd); return;
    }
    unsigned long long n2 = g_ddb_row_count(&res2);
    unsigned long long r;
    for (r = 0; r < n2; r++) {
      char *rowv = g_ddb_value_varchar(&res2, 0ULL, r);
      if (rowv) {
        char ev[8192]; int evl = snprintf(ev, 8192, "data: %s\n\n", rowv);
        char lhex[32]; int lh = snprintf(lhex, 32, "%x\r\n", evl);
        write(fd, lhex, lh);
        write(fd, ev, evl);
        write(fd, "\r\n", 2);
        g_ddb_free(rowv);
      }
    }
    write(fd, "0\r\n\r\n", 5);
    g_ddb_destroy_result(&res2);
    if (ct) g_ddb_free(ct);
    if (bod) g_ddb_free(bod);
    if (hsql) g_ddb_free(hsql);
    g_ddb_destroy_result(&res);
    close(fd);
    return;
  }
  if (hsql && hsql[0]) {
    ddb_result res2;
    int rc2 = g_ddb_query(con, hsql, &res2);
    if (rc2 != 0) {
      if (ct) g_ddb_free(ct);
      if (bod) g_ddb_free(bod);
      if (hsql) g_ddb_free(hsql);
      g_ddb_destroy_result(&res);
      char e[] = "HTTP/1.1 500 OK\r\nContent-Type: application/json\r\nContent-Length: 19\r\nConnection: close\r\n\r\n{\"error\":\"handler\"}";
      write(fd, e, sizeof(e)-1); close(fd); return;
    }
    unsigned long long n2 = g_ddb_row_count(&res2);
    if (n2 == 0) {
      if (ct) g_ddb_free(ct);
      if (bod) g_ddb_free(bod);
      if (hsql) g_ddb_free(hsql);
      g_ddb_destroy_result(&res2);
      g_ddb_destroy_result(&res);
      char e[] = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 22\r\nConnection: close\r\n\r\n{\"detail\":\"Not Found\"}";
      write(fd, e, sizeof(e)-1); close(fd); return;
    }
    char *hbody = g_ddb_value_varchar(&res2, 0ULL, 0ULL);
    if (bod) g_ddb_free(bod);
    final_bod = hbody;
    final_status = status;
    reason = (status == 201) ? "Created" : "OK";
    g_ddb_destroy_result(&res2);
    if (hsql) { g_ddb_free(hsql); hsql = 0; }
  }
  int bl = final_bod ? strlen(final_bod) : 0;
  char hdr[1024];
  int hl = snprintf(hdr, 1024, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", final_status, reason, ctype, bl);
  write(fd, hdr, hl);
  if(bl > 0){
    write(fd, final_bod, bl);
  }
  if (ct) g_ddb_free(ct);
  if (final_bod) g_ddb_free(final_bod);
  if (hsql) g_ddb_free(hsql);
  g_ddb_destroy_result(&res);
  close(fd);
}

static void *worker_main(void *arg){
  /* one PERSISTENT connection + prepared statements per worker; shellfs loaded once */
  void *con = 0;
  void *stmt = 0;
  if(g_ddb_connect(g_db, &con) != 0) return 0;
  /* OLTP tuning: each request is a tiny point query. DuckDB defaults threads to
     the core count, so 16 worker connections each launching a query that wants
     the whole pool thrash the global task scheduler down to ~1 effective core.
     threads=1 makes each query single-threaded (no morsel/scheduler overhead);
     the 16 CONNECTIONS supply the parallelism. This is global config, set once
     per worker (idempotent). */
  { ddb_result ign; g_ddb_query(con, "SET threads=1", &ign); g_ddb_destroy_result(&ign); }
  { ddb_result ign; g_ddb_query(con, "LOAD shellfs", &ign); g_ddb_destroy_result(&ign); }
  /* json + crypto are REQUIRED by handle_request: json for every route (to_json,
     JSON casts) and crypto for the auth path — _constant_time_str_equals is now an HMAC keyed-hash
     constant-time compare (crypto_hmac) and _verify_jwt_hs256 needs it too. Static /
     community-ext builds do NOT autoload these, so a worker conn that skips them 500s
     EVERY request with "crypto_hmac does not exist". Any new connection surface
     (workers, admin conns, replicas) MUST LOAD both. */
  { ddb_result ign; g_ddb_query(con, "LOAD json", &ign); g_ddb_destroy_result(&ign); }
  { ddb_result ign; g_ddb_query(con, "LOAD crypto", &ign); g_ddb_destroy_result(&ign); }
  /* HTTP CLIENT POLICY: curl_httpfs is the soldered default on EVERY worker conn.
     Handlers reading remote data over http (read_text/read_csv/read_parquet) then
     fetch CONCURRENTLY via connection pool + HTTP/2 + async IO. To revert to the
     stock SERIAL client, change the SET value below to "httplib" here AND in
     framework.sql (curl is double-wired on purpose — opting out takes intent). */
  { ddb_result ign; g_ddb_query(con, "LOAD curl_httpfs", &ign); g_ddb_destroy_result(&ign); }
  { ddb_result ign; g_ddb_query(con, "LOAD httpfs_timeout_retry", &ign); g_ddb_destroy_result(&ign); }
  { ddb_result ign; g_ddb_query(con, "SET httpfs_client_implementation=''curl''", &ign); g_ddb_destroy_result(&ign); }
  { ddb_result ign; g_ddb_query(con, "SET http_retries=3", &ign); g_ddb_destroy_result(&ign); }
  { ddb_result ign; g_ddb_query(con, "SET httpfs_retries_file_operation=3", &ign); g_ddb_destroy_result(&ign); }
  { ddb_result ign; g_ddb_query(con, "SET http_timeout=30000", &ign); g_ddb_destroy_result(&ign); }
  /* PURE TRACK hot path: prepare the handle_request TABLE macro once per worker.
     Everything — routing, validation, OpenAPI, handler templating — lives inside
     that one SQL call over the routes/param_schema registry. No brain_sql/exact_sql/
     route_* precompute tables: the router IS the query. This is the deliberately
     honest, slower path; the compiled-extension track moves routing into C to cross
     the per-request OLAP-query wall (edges.md #9). */
  const char *bsql = "SELECT * FROM handle_request(?, ?, ?, ?)";
  if(g_ddb_prepare(con, bsql, &stmt) != 0) return 0;
  for(;;){
    pthread_mutex_lock(&g_qm);
    while(g_qcount == 0){
      pthread_cond_wait(&g_qcv, &g_qm);
    }
    int fd = g_q[g_qhead]; g_qhead = (g_qhead + 1) % 4096; g_qcount = g_qcount - 1;
    pthread_mutex_unlock(&g_qm);
    handle_conn_on(con, stmt, fd);
  }
  return 0;
}

void *accept_loop(void *arg){
  for(;;){
    int c = accept(g_listen, 0, 0); if(c < 0) continue;
    { int nd = 1; setsockopt(c, 6, 1, &nd, 4); } /* TCP_NODELAY: flush the small JSON reply immediately, no Nagle coalescing wait */
    pthread_mutex_lock(&g_qm);
    if(g_qcount < 4096){
      g_q[g_qtail] = c; g_qtail = (g_qtail + 1) % 4096; g_qcount = g_qcount + 1;
      pthread_cond_signal(&g_qcv);
      pthread_mutex_unlock(&g_qm);
    } else {
      pthread_mutex_unlock(&g_qm);
      close(c);
    }
  }
  return 0;
}

const char *serve_brain(int port, const char *db_path){
  if(g_listen < 0){
    signal(13, (void*)1); /* SIG_IGN SIGPIPE: a write() to a client-closed socket must set EPIPE, never kill the duckdb process */
    g_listen = socket(2, 1, 0);
    int one = 1; setsockopt(g_listen, 0xffff, 0x0004, &one, 4);
    struct sockaddr_in a; char *pp = (char*)&a; int k; for(k = 0; k < 16; k++) pp[k] = 0;
    a.sin_len = 16; a.sin_family = 2;
    a.sin_port = (unsigned short)(((port & 0xff) << 8) | ((port >> 8) & 0xff));
    a.sin_addr.s_addr = 0;
    if(bind(g_listen, &a, 16) < 0){ close(g_listen); g_listen = -1; return "BIND_FAIL"; }
    listen(g_listen, 128);
    g_ddb_open = (ddb_open_t) resolve_sym("duckdb_open");
    g_ddb_connect = (ddb_connect_t) resolve_sym("duckdb_connect");
    g_ddb_disconnect = (ddb_disconnect_t) resolve_sym("duckdb_disconnect");
    g_ddb_close = (ddb_close_t) resolve_sym("duckdb_close");
    g_ddb_prepare = (ddb_prepare_t) resolve_sym("duckdb_prepare");
    g_ddb_bind_varchar = (ddb_bind_varchar_t) resolve_sym("duckdb_bind_varchar");
    g_ddb_execute_prepared = (ddb_execute_prepared_t) resolve_sym("duckdb_execute_prepared");
    g_ddb_destroy_prepare = (ddb_destroy_prepare_t) resolve_sym("duckdb_destroy_prepare");
    g_ddb_destroy_result = (ddb_destroy_result_t) resolve_sym("duckdb_destroy_result");
    g_ddb_row_count = (ddb_row_count_t) resolve_sym("duckdb_row_count");
    g_ddb_value_int32 = (ddb_value_int32_t) resolve_sym("duckdb_value_int32");
    g_ddb_value_varchar = (ddb_value_varchar_t) resolve_sym("duckdb_value_varchar");
    g_ddb_free = (ddb_free_t) resolve_sym("duckdb_free");
    g_ddb_query = (ddb_query_t) resolve_sym("duckdb_query");
    g_ddb_result_error = (ddb_result_error_t) resolve_sym("duckdb_result_error");
    g_ddb_execute_prepared_streaming = (ddb_execute_prepared_streaming_t) resolve_sym("duckdb_execute_prepared_streaming");
    g_ddb_fetch_chunk = (ddb_fetch_chunk_t) resolve_sym("duckdb_fetch_chunk");
    g_ddb_destroy_data_chunk = (ddb_destroy_data_chunk_t) resolve_sym("duckdb_destroy_data_chunk");
    g_ddb_chunk_get_size = (ddb_chunk_get_size_t) resolve_sym("duckdb_data_chunk_get_size");
    g_ddb_chunk_get_vector = (ddb_chunk_get_vector_t) resolve_sym("duckdb_data_chunk_get_vector");
    g_ddb_vector_get_data = (ddb_vector_get_data_t) resolve_sym("duckdb_vector_get_data");
    g_ddb_vector_get_validity = (ddb_vector_get_validity_t) resolve_sym("duckdb_vector_get_validity");
    if(!g_ddb_open || !g_ddb_connect || !g_ddb_disconnect || !g_ddb_close ||
       !g_ddb_prepare || !g_ddb_bind_varchar || !g_ddb_execute_prepared || !g_ddb_destroy_prepare ||
       !g_ddb_destroy_result || !g_ddb_row_count || !g_ddb_value_int32 || !g_ddb_value_varchar || !g_ddb_free || !g_ddb_query){
      close(g_listen); g_listen = -1; return "SYM_FAIL";
    }
    int oret = g_ddb_open(db_path, &g_db);
    if(oret != 0){
      close(g_listen); g_listen = -1; g_db = 0; return "DBOPEN_FAIL";
    }
  }
  if(!g_pool_started){
    pthread_mutex_init(&g_qm, 0);
    pthread_cond_init(&g_qcv, 0);
    int k;
    for(k=0; k < NWORKERS; k++){
      pthread_t w;
      if(pthread_create(&w, 0, worker_main, 0) == 0) pthread_detach(w);
    }
    g_pool_started = 1;
  }
  pthread_t srv;
  if(pthread_create(&srv, 0, accept_loop, 0) != 0) return "SPAWN_FAIL";
  pthread_detach(srv);
  return "BRAIN_LISTENING pool=16";
}

int block_forever(int x){ for(;;) usleep(1000000); return 0; }
',
  symbol := 'serve_brain', sql_name := 'serve_brain',
  return_type := 'varchar', arg_types := ['i32','varchar'], stability := 'volatile', library := 'c');

SELECT ok, code FROM tcc_module(mode := 'quick_compile',
  source := 'int usleep(unsigned int); int block_forever(int x){ for(;;) usleep(1000000); return 0; }',
  symbol := 'block_forever', sql_name := 'block_forever',
  return_type := 'i32', arg_types := ['i32'], stability := 'volatile', library := 'c');
