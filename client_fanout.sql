INSTALL ducktinycc FROM community; LOAD ducktinycc;

-- ============================================================================
-- quackapi — client_fanout: TRUE parallel self-dispatch.
--
-- http_client (http_get/http_post_form) serializes every HTTP call behind one
-- internal path: threads=8 gave ZERO speedup over threads=1 (28.70s vs 29.56s
-- for 4096 reqs). So the fan-out itself must own its threads.
--
-- http_fanout(port, n, nthreads) -> int : spawns `nthreads` C worker threads,
-- statically partitions n requests across them, each thread opens its own
-- socket + connect() to 127.0.0.1:port, fires GET /users/1?i=K, reads the
-- response, counts the 200s. Returns total 200 count. One SQL call => up to
-- `nthreads` requests in flight at once = real high-level parallelism.
-- Mirrors the server's proven worker-pool socket code (serve_brain.sql).
-- ============================================================================

SELECT ok, code FROM tcc_module(mode := 'quick_compile',
  source := 'struct in_addr { unsigned int s_addr; };
struct sockaddr_in { unsigned char sin_len; unsigned char sin_family; unsigned short sin_port; struct in_addr sin_addr; char sin_zero[8]; };
int socket(int,int,int);
int connect(int,const struct sockaddr_in*,unsigned int);
long read(int,void*,unsigned long);
long write(int,const void*,unsigned long);
int close(int);
int snprintf(char*,unsigned long,const char*,...);
char *strstr(const char*,const char*);
typedef void* pthread_t;
int pthread_create(pthread_t*,void*,void*(*)(void*),void*);
int pthread_join(pthread_t,void**);

#define MAXT 256
static int g_port; static int g_n; static int g_per;
static int g_cnt[MAXT];

void *fan_worker(void *arg){
  int t = (int)(long)arg;
  int lo = t * g_per;
  int hi = lo + g_per;
  if(hi > g_n) hi = g_n;
  int c = 0;
  int k;
  for(k = lo; k < hi; k++){
    int fd = socket(2, 1, 0);
    if(fd < 0) continue;
    struct sockaddr_in a;
    char *pp = (char*)&a; int z; for(z = 0; z < 16; z++) pp[z] = 0;
    a.sin_len = 16; a.sin_family = 2;
    a.sin_port = (unsigned short)(((g_port & 0xff) << 8) | ((g_port >> 8) & 0xff));
    a.sin_addr.s_addr = 0x0100007f; /* 127.0.0.1 in network byte order */
    if(connect(fd, &a, 16) == 0){
      char req[256];
      int rl = snprintf(req, 256, "GET /users/1?i=%d HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n", k);
      write(fd, req, rl);
      char buf[2048];
      long rn = read(fd, buf, 2047);
      if(rn > 0){ buf[rn] = 0; if(strstr(buf, " 200 ")) c++; }
    }
    close(fd);
  }
  g_cnt[t] = c;
  return 0;
}

int http_fanout(int port, int n, int nthreads){
  if(nthreads < 1) nthreads = 1;
  if(nthreads > MAXT) nthreads = MAXT;
  g_port = port; g_n = n;
  g_per = (n + nthreads - 1) / nthreads;
  int i;
  for(i = 0; i < nthreads; i++) g_cnt[i] = 0;
  pthread_t th[MAXT];
  for(i = 0; i < nthreads; i++){
    pthread_create(&th[i], 0, fan_worker, (void*)(long)i);
  }
  for(i = 0; i < nthreads; i++){
    pthread_join(th[i], 0);
  }
  int total = 0;
  for(i = 0; i < nthreads; i++) total += g_cnt[i];
  return total;
}
',
  symbol := 'http_fanout', sql_name := 'http_fanout',
  return_type := 'i32', arg_types := ['i32','i32','i32'], stability := 'volatile', library := 'c');
