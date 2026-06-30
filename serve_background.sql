-- ============================================================================
-- quackapi — serve_background: the SAME server as serve_forever, but the accept
-- loop runs on a BACKGROUND pthread and the function RETURNS IMMEDIATELY.
--
-- This is the ONE-LINE difference that makes a server "just run" like
-- httpserver's http_serve() or airport: the start call spawns a thread that
-- owns the socket + the forever-loop, then hands your session back. The thing
-- that "just runs" is that C accept-loop on its own thread — never a query.
--
--   serve_forever(port):    runs for(;;)accept() INLINE  -> never returns (session hangs)
--   serve_background(port):  spawns accept_loop on a pthread -> returns instantly
--
-- Same handler, same concurrency (pthread per connection). Each handler sleeps
-- 300ms so concurrency stays visible.
-- macOS/BSD sockaddr (sin_len + 1-byte family); forward-declare libc; library:='c'.
-- ============================================================================

INSTALL ducktinycc FROM community; LOAD ducktinycc;

-- ---- serve_background(port) -> returns "LISTENING_IN_BACKGROUND" immediately --
SELECT ok, code FROM tcc_module(mode := 'quick_compile',
  source := 'struct in_addr { unsigned int s_addr; };
struct sockaddr_in { unsigned char sin_len; unsigned char sin_family; unsigned short sin_port; struct in_addr sin_addr; char sin_zero[8]; };
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
  usleep(300000);  /* 300ms of "work" so concurrency is observable */
  int bl=snprintf(body,1024,"{\"routed_by\":\"pure DuckDB C thread (BACKGROUND loop)\",\"method\":\"%s\",\"path\":\"%s\"}",method,path);
  int ol=snprintf(out,66000,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: %d\r\n\r\n%s",bl,body);
  write(fd,out,ol); close(fd);
  return 0;
}
void *accept_loop(void *arg){           /* the forever-loop — on its OWN thread */
  for(;;){
    int c=accept(g_listen,0,0); if(c<0) continue;
    pthread_t t; if(pthread_create(&t,0,handle_conn,(void*)(long)c)==0) pthread_detach(t); else close(c);
  }
  return 0;
}
const char *serve_background(int port){
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
  pthread_t srv;
  if(pthread_create(&srv,0,accept_loop,0)!=0) return "SPAWN_FAIL";  /* hand the loop to a thread */
  pthread_detach(srv);
  return "LISTENING_IN_BACKGROUND";     /* ...and RETURN. The session is free now. */
}',
  symbol := 'serve_background', sql_name := 'serve_background',
  return_type := 'varchar', arg_types := ['i32'], stability := 'volatile', library := 'c');

-- ---- block_forever: keep the PROCESS alive (stands in for an idle prompt) ----
-- In an interactive `duckdb` session you would just sit at the `D ` prompt and
-- the process stays up. Running headless, this UDF is that idle prompt: the MAIN
-- thread parks here doing nothing, while the BACKGROUND thread serves requests.
SELECT ok, code FROM tcc_module(mode := 'quick_compile',
  source := 'int usleep(unsigned int); int block_forever(int x){ for(;;) usleep(1000000); return 0; }',
  symbol := 'block_forever', sql_name := 'block_forever',
  return_type := 'i32', arg_types := ['i32'], stability := 'volatile', library := 'c');

-- 1) Start it. This RETURNS immediately — proving the loop is on another thread.
SELECT serve_background(18082) AS server_start;

-- 2) The main thread is FREE: it runs ordinary queries right after starting the
--    server. If serving were "a query that runs", we could never reach this line.
SELECT 'main thread is free while the server runs' AS proof, 2 + 2 AS arithmetic;

-- 3) Park the main thread so the process (and its background server) stays alive.
SELECT block_forever(0) AS parked;
