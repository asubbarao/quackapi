-- ============================================================================
-- quackapi — a PURE-DuckDB HTTP listener (no Python, no separate process)
--
-- Kills the "path-on-wire" boundary: ducktinycc JIT-compiles C socket syscalls
-- INSIDE the DuckDB process; SQL drives the accept loop. C does accept/read/
-- write; SQL does routing/validation/serialization. They talk only via an fd
-- integer + the request/response strings. C never calls back into DuckDB.
--
-- PROVEN: a dumb `curl GET /users/123?x=1` received SQL-generated JSON.
--
-- Platform note: the sockaddr layout below is macOS/BSD (sin_len + 1-byte
-- sin_family). On Linux, sin_family is 2 bytes and there is no sin_len — guard
-- with the right struct per-OS. We forward-declare libc ourselves (TinyCC on
-- macOS can't find SDK headers) and link libc with `library := 'c'`.
-- Caveats: single-threaded; `PRAGMA threads=1`; no sandbox (a C bug crashes the
-- process — check every return); one request per statement (see LOOP below).
-- ============================================================================

INSTALL ducktinycc FROM community; LOAD ducktinycc;
PRAGMA threads=1;  -- keep the blocking accept loop on one thread

-- ---- accept_one(port) -> "<client_fd>\n<raw http request>" ------------------
-- Creates+binds+listens the first time (listen fd kept in a C static), then
-- blocks on accept(), reads the request, returns "fd\n<request bytes>".
SELECT ok, code FROM tcc_module(mode := 'quick_compile',
  source := 'struct in_addr { unsigned int s_addr; };
struct sockaddr { unsigned char sa_len; unsigned char sa_family; char sa_data[14]; };
struct sockaddr_in { unsigned char sin_len; unsigned char sin_family; unsigned short sin_port; struct in_addr sin_addr; char sin_zero[8]; };
int socket(int,int,int); int setsockopt(int,int,int,const void*,unsigned int);
int bind(int,const struct sockaddr*,unsigned int); int listen(int,int);
int accept(int,struct sockaddr*,unsigned int*); long read(int,void*,unsigned long);
int close(int); int snprintf(char*,unsigned long,const char*,...);
static int g_listen=-1; static char g_buf[65536];
const char *accept_one(int port){
  if(g_listen<0){
    g_listen=socket(2,1,0);                         /* AF_INET, SOCK_STREAM */
    int one=1; setsockopt(g_listen,0xffff,0x0004,&one,4); /* SOL_SOCKET, SO_REUSEADDR */
    struct sockaddr_in a; int i; char *pp=(char*)&a; for(i=0;i<16;i++) pp[i]=0;
    a.sin_len=16; a.sin_family=2;
    a.sin_port=(unsigned short)(((port&0xff)<<8)|((port>>8)&0xff));  /* htons */
    a.sin_addr.s_addr=0;                            /* INADDR_ANY */
    if(bind(g_listen,(struct sockaddr*)&a,16)<0){ close(g_listen); g_listen=-1; return "-1\nBIND_FAIL"; }
    listen(g_listen,16);
  }
  int c=accept(g_listen,0,0); if(c<0) return "-1\nACCEPT_FAIL";
  int hn=snprintf(g_buf,65536,"%d\n",c);
  long n=read(c,g_buf+hn,65535-hn); if(n<0) n=0; g_buf[hn+n]=0;
  return g_buf;
}',
  symbol := 'accept_one', sql_name := 'accept_one',
  return_type := 'varchar', arg_types := ['i32'], stability := 'volatile', library := 'c');

-- ---- respond(client_fd, response) -> 1/0 ------------------------------------
SELECT ok, code FROM tcc_module(mode := 'quick_compile',
  source := 'unsigned long strlen(const char*); long write(int,const void*,unsigned long); int close(int);
int respond(int fd,const char *resp){ unsigned long L=strlen(resp); long w=write(fd,resp,L); close(fd); return (w==(long)L)?1:0; }',
  symbol := 'respond', sql_name := 'respond',
  return_type := 'i32', arg_types := ['i32','varchar'], stability := 'volatile', library := 'c');

-- ---- serve ONE request ------------------------------------------------------
-- C accepts+reads -> SQL parses+routes -> C writes. Replace the json_object
-- below with a call to handle_request(method, path, ...) from framework.sql.
WITH r     AS (SELECT accept_one(18080) AS raw),
     s     AS (SELECT string_split(raw, chr(10)) AS parts FROM r),
     p     AS (SELECT try_cast(parts[1] AS INTEGER) AS fd, parts[2] AS line FROM s),
     route AS (SELECT fd, split_part(line,' ',1) AS method,
                      split_part(split_part(line,' ',2),'?',1) AS path FROM p),
     body  AS (SELECT fd, method, path,
                 json_object('routed_by','pure DuckDB SQL','method',method,'path',path)::VARCHAR AS j FROM route),
     resp  AS (SELECT fd,
                 'HTTP/1.1 200 OK'||chr(13)||chr(10)||'Content-Type: application/json'||chr(13)||chr(10)||
                 'Connection: close'||chr(13)||chr(10)||chr(13)||chr(10)||j AS http FROM body)
SELECT respond(fd, http) AS wrote_ok FROM resp;

-- ---- LOOP (serving continuously) -------------------------------------------
-- DuckDB has no WHILE, and a `FROM range(N)` driver batches 2048 accepts per
-- vector (deadlock: clients wait for responses that don't come until the chunk
-- fills). So drive ONE request per statement and re-invoke:
--   * a DuckDB scheduled task / cron firing this statement (pure DuckDB), or
--   * `while true; do duckdb < listener_ducktinycc.sql ; done` (loop is shell,
--     each iteration is pure DuckDB), or
--   * a C accept loop with pthreads (real concurrency) — bigger lift, future.
-- The listener itself (path off the wire -> SQL -> response) is 100% DuckDB;
-- only the outer "keep going" needs a driver.
