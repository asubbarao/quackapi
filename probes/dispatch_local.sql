-- ============================================================================
-- probes/dispatch_local.sql : adapted dispatch for EDGE-PROBES on port 9497
-- Token: quackapi_edges_probe , db: quackapi_edges.db
-- Copy of dispatch.sql core, with TOKEN and port changed for isolated run.
-- Include this (cat or .read equivalent via combined) before using dispatch().
-- ============================================================================

INSTALL ducktinycc FROM community; LOAD ducktinycc;

-- C MODULE 1 — dispatch_fanout (parallel writes via raw sockets to harbor /sql)
SELECT ok, code, message FROM tcc_module(mode := 'quick_compile',
  source := 'struct in_addr { unsigned int s_addr; };
struct sockaddr_in { unsigned char sin_len; unsigned char sin_family; unsigned short sin_port; struct in_addr sin_addr; char sin_zero[8]; };
int socket(int,int,int);
int connect(int,const struct sockaddr_in*,unsigned int);
long read(int,void*,unsigned long);
long write(int,const void*,unsigned long);
int close(int);
int snprintf(char*,unsigned long,const char*,...);
char *strstr(const char*,const char*);
unsigned long strlen(const char*);
int usleep(unsigned int);
typedef void* pthread_t;
int pthread_create(pthread_t*,void*,void*(*)(void*),void*);
int pthread_join(pthread_t,void**);

#define MAXN 256
#define RESPSZ 16384
#define INSZ (4*1024*1024)
#define OUTSZ (4*1024*1024)
#define REQSZ 66560
#define TOKEN "quackapi_edges_probe"

static char g_inbuf[INSZ];
static char *g_body[MAXN];
static char g_resp[MAXN][RESPSZ];
static char g_out[OUTSZ];
static int g_port, g_n, g_per, g_retries;

static int build_req(const char *body, char *req){
  return snprintf(req, REQSZ,
    "POST /sql HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
    TOKEN, (int)strlen(body), body);
}

static void do_post(const char *req, int rl, int port, char *resp, int respcap){
  resp[0]=0;
  int fd = socket(2,1,0);
  if(fd < 0) return;
  struct sockaddr_in a;
  char *pp=(char*)&a; int z; for(z=0;z<16;z++) pp[z]=0;
  a.sin_len=16; a.sin_family=2;
  a.sin_port=(unsigned short)(((port & 255)<<8) | ((port>>8) & 255));
  a.sin_addr.s_addr=0x0100007f;
  if(connect(fd,&a,16)==0){
    write(fd, req, rl);
    int off=0; long rn;
    while((rn=read(fd, resp+off, respcap-1-off))>0){ off+=rn; if(off>=respcap-1) break; }
    resp[off]=0;
  }
  close(fd);
}

static void clean_body(char *resp, char *out, int outcap){
  int o=0;
  char *b = strstr(resp, "\r\n\r\n");
  char *hdr_chunked = strstr(resp, "chunked");
  if(b){ *b = 0; }
  int chunked = (hdr_chunked && b && hdr_chunked < b) ? 1 : 0;
  char *body = b ? b+4 : resp;
  if(chunked){
    char *p = body;
    for(;;){
      int len=0, any=0;
      while(*p){
        char c=*p; int d=-1;
        if(c>=48 && c<=57) d=c-48;
        else if(c>=97 && c<=102) d=c-97+10;
        else if(c>=65 && c<=70) d=c-65+10;
        else break;
        len=len*16+d; any=1; p++;
      }
      while(*p && *p!=10) p++;
      if(*p==10) p++;
      if(!any || len==0) break;
      int j;
      for(j=0;j<len && *p;j++){ if(o<outcap-1) out[o++]=*p; p++; }
      if(*p==13) p++;
      if(*p==10) p++;
    }
  } else {
    int j;
    for(j=0; body[j]; j++){ if(o<outcap-1) out[o++]=body[j]; }
  }
  out[o]=0;
}

void *disp_worker(void *arg){
  int t = (int)(long)arg;
  int lo = t*g_per; int hi = lo+g_per; if(hi>g_n) hi=g_n;
  int k;
  char resp[RESPSZ];
  for(k=lo;k<hi;k++){
    char req[REQSZ];
    int rl = build_req(g_body[k], req);
    int attempt=0;
    for(;;){
      do_post(req, rl, g_port, resp, RESPSZ);
      if(g_retries>0 && attempt<g_retries && strstr(resp, "Conflict")){
        usleep((unsigned int)((attempt+1)*1500 + t*200));
        attempt++;
        continue;
      }
      break;
    }
    clean_body(resp, g_resp[k], RESPSZ);
  }
  return 0;
}

const char *dispatch_fanout(const char *joined, int nthreads, int port, int max_retries){
  int L=0; while(joined[L] && L<INSZ-1){ g_inbuf[L]=joined[L]; L++; } g_inbuf[L]=0;
  int idx=1; g_body[0]=g_inbuf; int p;
  for(p=0;p<L;p++){ if((unsigned char)g_inbuf[p]==30){ g_inbuf[p]=0; if(idx<MAXN) g_body[idx++]=g_inbuf+p+1; } }
  g_n=idx; g_port=port; g_retries=max_retries;
  if(nthreads<1) nthreads=1; if(nthreads>MAXN) nthreads=MAXN;
  g_per=(g_n+nthreads-1)/nthreads;
  pthread_t th[MAXN]; int i;
  for(i=0;i<nthreads;i++) pthread_create(&th[i],0,disp_worker,(void*)(long)i);
  for(i=0;i<nthreads;i++) pthread_join(th[i],0);
  int o=0,k;
  for(k=0;k<g_n;k++){
    if(k>0) g_out[o++]=30;
    int j; char *b=g_resp[k];
    for(j=0;b[j];j++){ if(o>=OUTSZ-2) break; if((unsigned char)b[j]==30) continue; g_out[o++]=b[j]; }
  }
  g_out[o]=0;
  return g_out;
}
',
  symbol := 'dispatch_fanout', sql_name := 'dispatch_fanout',
  return_type := 'varchar', arg_types := ['varchar','i32','i32','i32'],
  stability := 'volatile', library := 'c');

-- C MODULE 2 — async (not used in these probes but for completeness)
SELECT ok, code, message FROM tcc_module(mode := 'quick_compile',
  source := 'struct in_addr { unsigned int s_addr; };
struct sockaddr_in { unsigned char sin_len; unsigned char sin_family; unsigned short sin_port; struct in_addr sin_addr; char sin_zero[8]; };
int socket(int,int,int);
int connect(int,const struct sockaddr_in*,unsigned int);
long read(int,void*,unsigned long);
long write(int,const void*,unsigned long);
int close(int);
int snprintf(char*,unsigned long,const char*,...);
unsigned long strlen(const char*);
void *malloc(unsigned long);
void free(void*);
typedef void* pthread_t;
int pthread_create(pthread_t*,void*,void*(*)(void*),void*);
int pthread_detach(pthread_t);

#define BODYSZ 65536
#define REQSZ 66560
#define TOKEN "quackapi_edges_probe"

struct asyncjob { char body[BODYSZ]; int port; };

static void a_post(const char *req, int rl, int port){
  int fd = socket(2,1,0);
  if(fd < 0) return;
  struct sockaddr_in a;
  char *pp=(char*)&a; int z; for(z=0;z<16;z++) pp[z]=0;
  a.sin_len=16; a.sin_family=2;
  a.sin_port=(unsigned short)(((port & 255)<<8) | ((port>>8)&255));
  a.sin_addr.s_addr=0x0100007f;
  if(connect(fd,&a,16)==0){
    write(fd, req, rl);
    char buf[256]; read(fd, buf, 255);
  }
  close(fd);
}

void *async_worker(void *arg){
  struct asyncjob *j = (struct asyncjob*)arg;
  char req[REQSZ];
  int rl = snprintf(req, REQSZ,
    "POST /sql HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
    TOKEN, (int)strlen(j->body), j->body);
  a_post(req, rl, j->port);
  free(j);
  return 0;
}

int dispatch_async_fire(const char *body, int port){
  struct asyncjob *j = (struct asyncjob*)malloc(sizeof(struct asyncjob));
  if(!j) return -1;
  int i=0; while(body[i] && i<BODYSZ-1){ j->body[i]=body[i]; i++; } j->body[i]=0;
  j->port=port;
  pthread_t th;
  if(pthread_create(&th,0,async_worker,j)!=0){ free(j); return -1; }
  pthread_detach(th);
  return 0;
}
',
  symbol := 'dispatch_async_fire', sql_name := 'dispatch_async_fire',
  return_type := 'i32', arg_types := ['varchar','i32'],
  stability := 'volatile', library := 'c');

-- Local port + macros (overridden for 9497)
CREATE OR REPLACE MACRO loopback_port() AS 9497;

CREATE OR REPLACE MACRO exec_select(sql) AS TABLE (
  FROM json_execute_serialized_sql(json_serialize_sql(sql))
);

CREATE OR REPLACE MACRO dispatch(sqls, nthreads := -1, max_retries := 0) AS TABLE (
  WITH raw AS (
    SELECT string_split(
      dispatch_fanout(
        array_to_string(list_transform(sqls, lambda s: to_json({sql: s})), chr(30)),
        CASE WHEN nthreads < 0
             THEN least(len(sqls), current_setting('threads')::BIGINT)::INTEGER
             ELSE nthreads END,
        loopback_port(), max_retries),
      chr(30)) AS parts
  )
  SELECT u.ord - 1 AS idx,
         from_json(u.part, '{"ok":"BOOLEAN"}').ok AS ok,
         from_json(u.part, '{"rowCount":"BIGINT"}')."rowCount" AS row_count,
         u.part AS response
  FROM raw, unnest(raw.parts) WITH ORDINALITY AS u(part, ord)
);

CREATE OR REPLACE MACRO dispatch_retry(sqls, max_retries) AS TABLE (
  FROM dispatch(sqls, max_retries := max_retries)
);

CREATE OR REPLACE MACRO dispatch_async(sql) AS (
  dispatch_async_fire(to_json({sql: sql}), loopback_port())
);
