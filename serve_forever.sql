-- ============================================================================
-- quackapi — serve_forever: a PERSISTENT, THREADED, in-process HTTP server.
--
-- Fixes two caveats of the one-shot listener at once:
--   * caveat #3 (one request per statement): the accept loop is a real C
--     while(1) — no DuckDB vectorized FROM range(N) batching, no shell loop.
--     ONE statement (`SELECT serve_forever(port)`) never returns; it IS the
--     server process.
--   * caveat #1 (single-threaded): pthread_create per accepted connection, so
--     concurrent clients are served in parallel (proven: pthreads run inside
--     ducktinycc).
--
-- Milestone A: routes IN C (returns JSON directly) to prove the concurrency
-- model. Milestone C swaps the in-C body for a self-dispatch to the SQL brain
-- (handle_request on a quack listener), restoring the pure-SQL-brain thesis.
--
-- Each handler sleeps 300ms to make concurrency observable: N sequential
-- requests would take N*0.3s; N concurrent take ~0.3s.
-- macOS/BSD sockaddr (sin_len + 1-byte family); forward-declare libc, link with
-- library:='c' (TinyCC can't find macOS SDK headers).
-- ============================================================================

INSTALL ducktinycc FROM community; LOAD ducktinycc;

SELECT ok, code FROM tcc_module(mode := 'quick_compile',
  source := 'struct in_addr { unsigned int s_addr; };
struct sockaddr_in { unsigned char sin_len; unsigned char sin_family; unsigned short sin_port; struct in_addr sin_addr; char sin_zero[8]; };
struct sockaddr;
int socket(int,int,int); int setsockopt(int,int,int,const void*,unsigned int);
int bind(int,const struct sockaddr_in*,unsigned int); int listen(int,int);
int accept(int,void*,void*); long read(int,void*,unsigned long);
long write(int,const void*,unsigned long); int close(int);
int snprintf(char*,unsigned long,const char*,...); int usleep(unsigned int);
typedef void* pthread_t; int pthread_create(pthread_t*,void*,void*(*)(void*),void*); int pthread_detach(pthread_t);
static int g_listen=-1;
void *handle_conn(void *arg){
  int fd=(int)(long)arg;
  char req[65536]; char body[1024]; char out[66000];
  long n=read(fd,req,65535); if(n<0) n=0; req[n]=0;
  char method[16]; char path[1024]; int i=0,j=0;
  while(req[i] && req[i]!=32 && i<15){ method[i]=req[i]; i++; } method[i]=0;
  if(req[i]==32) i++;
  while(req[i] && req[i]!=32 && req[i]!=63 && j<1023){ path[j++]=req[i++]; } path[j]=0;
  usleep(300000);  /* simulate 300ms of work so concurrency is observable */
  int bl=snprintf(body,1024,"{\"routed_by\":\"pure DuckDB C thread\",\"method\":\"%s\",\"path\":\"%s\"}",method,path);
  int ol=snprintf(out,66000,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: %d\r\n\r\n%s",bl,body);
  write(fd,out,ol); close(fd);
  return 0;
}
const char *serve_forever(int port){
  if(g_listen<0){
    g_listen=socket(2,1,0);
    int one=1; setsockopt(g_listen,0xffff,0x0004,&one,4);
    struct sockaddr_in a; char *pp=(char*)&a; int k; for(k=0;k<16;k++) pp[k]=0;
    a.sin_len=16; a.sin_family=2;
    a.sin_port=(unsigned short)(((port&0xff)<<8)|((port>>8)&0xff));
    a.sin_addr.s_addr=0;
    if(bind(g_listen,&a,16)<0){ close(g_listen); g_listen=-1; return "BIND_FAIL"; }
    listen(g_listen,128);
  }
  for(;;){
    int c=accept(g_listen,0,0); if(c<0) continue;
    pthread_t t; if(pthread_create(&t,0,handle_conn,(void*)(long)c)==0) pthread_detach(t); else close(c);
  }
  return "STOPPED";
}',
  symbol := 'serve_forever', sql_name := 'serve_forever',
  return_type := 'varchar', arg_types := ['i32'], stability := 'volatile', library := 'c');

-- This statement never returns. It IS the server. Stop it by killing the proc.
SELECT serve_forever(18080) AS server;
