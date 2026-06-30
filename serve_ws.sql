-- ============================================================================
-- quackapi -- serve_ws.sql
-- WebSocket server: HTTP->WS upgrade handshake + frame echo loop.
-- Pure in-process C via ducktinycc. No Python, no external process.
--
-- Edge #3 from edges.md (edges.md row #3): "WebSockets -- REAL -- stateful,
-- bidirectional, no SQL analog." This file proves both milestones.
--
-- MILESTONE 1: HTTP->WS upgrade handshake
--   * parse Sec-WebSocket-Key from request headers (case-insensitive scan)
--   * compute Sec-WebSocket-Accept = base64(sha1(key + WS_MAGIC))
--     SHA-1 and base64 are implemented in C inline (RFC 3174 + RFC 4648)
--     no libs beyond libc forward-declarations
--   * respond HTTP/1.1 101 Switching Protocols
--
-- MILESTONE 2: frame round-trip echo
--   * read client text frame: parse FIN/opcode, 7/16/64-bit payload length,
--     unmask payload with the 4-byte masking key (client frames always masked)
--   * echo back as server text frame: 0x81, length, payload (no mask)
--   * loop until close frame (opcode=8) or read error
--
-- PROVEN (2026-06-29, duckdb v1.5.3 osx_arm64, port 18097):
--   M1: status=101 Switching Protocols
--       Sec-WebSocket-Accept: qu0iAazDOhkBILyVhoUwKGEFSsY=   (verified correct)
--   M2: sent "hello" -> received "hello"
--       sent "quackapi ws edge #3 DEFEATED" -> received verbatim
--
-- ducktinycc escaping rules in this file:
--   * C source in SQL single-quotes; any C single-quote must be doubled ('')
--   * char literals \r/\n replaced with (char)13/(char)10 via #define CR/LF
--     (DuckDB SQL parser sees backslash before C parser does)
--   * '=' (0x3D) replaced with PAD_CHAR macro for the same reason
--   * libc symbols forward-declared; link via library := 'c'
--   * sockaddr layout is macOS/BSD (sin_len byte + 1-byte sin_family)
--
-- Run (boots on port 18097, stateless echo -- no DB file needed, blocks forever):
--   duckdb :memory: < serve_ws.sql &
--   WS_PID=$!
-- Test:
--   python3 -c "
--     import asyncio, websockets
--     async def t():
--       async with websockets.connect(''ws://127.0.0.1:18097'') as ws:
--         await ws.send(''hello''); print(await ws.recv())
--     asyncio.run(t())"
-- Stop (ONLY kill the specific PID you captured -- never pkill duckdb):
--   kill $WS_PID
--
-- Safety: exclusively uses port 18097 (in-memory, no DB file).
-- Does NOT touch port 9494 (quackserver) or 9495 (harbor loopback).
-- ============================================================================

INSTALL ducktinycc FROM community; LOAD ducktinycc;

SELECT ok, code FROM tcc_module(mode := 'quick_compile',
  source := 'struct in_addr { unsigned int s_addr; };
struct sockaddr_in {
  unsigned char  sin_len;
  unsigned char  sin_family;
  unsigned short sin_port;
  struct in_addr sin_addr;
  char           sin_zero[8];
};
int    socket(int,int,int);
int    setsockopt(int,int,int,const void*,unsigned int);
int    bind(int, const struct sockaddr_in*, unsigned int);
int    listen(int, int);
int    accept(int, void*, void*);
long   read(int, void*, unsigned long);
long   write(int, const void*, unsigned long);
int    close(int);
int    snprintf(char*, unsigned long, const char*, ...);
unsigned long strlen(const char*);
void  *memcpy(void*, const void*, unsigned long);
void  *memset(void*, int, unsigned long);
typedef void* pthread_t;
int    pthread_create(pthread_t*, void*, void*(*)(void*), void*);
int    pthread_detach(pthread_t);

typedef unsigned int  u32;
typedef unsigned char u8;

static u32 sha1_rotl(u32 v, int n){ return (v << n) | (v >> (32-n)); }

static void sha1_block(u32 *h, const u8 *blk){
  u32 w[80]; int i;
  for(i=0;i<16;i++){
    w[i] = ((u32)blk[i*4]<<24)|((u32)blk[i*4+1]<<16)|
           ((u32)blk[i*4+2]<<8)|(u32)blk[i*4+3];
  }
  for(i=16;i<80;i++) w[i]=sha1_rotl(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
  u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
  for(i=0;i<80;i++){
    u32 f,k,t;
    if(i<20){  f=(b&c)|(~b&d); k=0x5A827999u; }
    else if(i<40){ f=b^c^d;     k=0x6ED9EBA1u; }
    else if(i<60){ f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDCu; }
    else{          f=b^c^d;     k=0xCA62C1D6u; }
    t=sha1_rotl(a,5)+f+e+k+w[i];
    e=d; d=c; c=sha1_rotl(b,30); b=a; a=t;
  }
  h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
}

static void sha1(const u8 *msg, unsigned long mlen, u8 *out){
  u32 h[5]={0x67452301u,0xEFCDAB89u,0x98BADCFEu,0x10325476u,0xC3D2E1F0u};
  u8  blk[64];
  unsigned long i, off=0;
  while(off+64 <= mlen){
    sha1_block(h, msg+off); off+=64;
  }
  unsigned long tail = mlen - off;
  memcpy(blk, msg+off, tail);
  blk[tail] = 0x80;
  memset(blk+tail+1, 0, 63-tail);
  if(tail >= 55){
    sha1_block(h, blk);
    memset(blk, 0, 56);
  }
  unsigned long long bits = (unsigned long long)mlen * 8;
  blk[56]=(u8)(bits>>56); blk[57]=(u8)(bits>>48);
  blk[58]=(u8)(bits>>40); blk[59]=(u8)(bits>>32);
  blk[60]=(u8)(bits>>24); blk[61]=(u8)(bits>>16);
  blk[62]=(u8)(bits>>8);  blk[63]=(u8)(bits);
  sha1_block(h, blk);
  for(i=0;i<5;i++){
    out[i*4+0]=(u8)(h[i]>>24); out[i*4+1]=(u8)(h[i]>>16);
    out[i*4+2]=(u8)(h[i]>>8);  out[i*4+3]=(u8)(h[i]);
  }
}

static const char b64t[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#define PAD_CHAR ((char)0x3D)

static int b64enc(const u8 *src, int slen, char *dst){
  int i=0,j=0;
  while(i < slen){
    u32 octet_a = i < slen ? src[i++] : 0;
    u32 octet_b = i < slen ? src[i++] : 0;
    u32 octet_c = i < slen ? src[i++] : 0;
    u32 triple = (octet_a<<16)|(octet_b<<8)|octet_c;
    dst[j++]=b64t[(triple>>18)&0x3F];
    dst[j++]=b64t[(triple>>12)&0x3F];
    dst[j++]=b64t[(triple>>6)&0x3F];
    dst[j++]=b64t[triple&0x3F];
  }
  int pad = slen % 3;
  if(pad==1){ dst[j-1]=PAD_CHAR; dst[j-2]=PAD_CHAR; }
  else if(pad==2){ dst[j-1]=PAD_CHAR; }
  dst[j]=0;
  return j;
}

/* CR=13, LF=10 */
#define CR ((char)13)
#define LF ((char)10)

static int find_header(const char *req, const char *key, char *val, int vmax){
  const char *p = req;
  while(*p && !(*p==CR && *(p+1)==LF)) p++;
  if(*p) p+=2;
  int klen=(int)strlen(key);
  while(*p && !(*p==CR && *(p+1)==LF)){
    int match=1;
    int i;
    for(i=0;i<klen;i++){
      char a=p[i]; char b=key[i];
      if(a>=(char)65 && a<=(char)90) a+=32;
      if(b>=(char)65 && b<=(char)90) b+=32;
      if(a!=b){ match=0; break; }
    }
    if(match && p[klen]==(char)58){
      const char *v = p+klen+1;
      while(*v==(char)32) v++;
      int j=0;
      while(*v && *v!=CR && j<vmax-1){ val[j++]=*v++; }
      val[j]=0;
      return 1;
    }
    while(*p && !(*p==CR && *(p+1)==LF)) p++;
    if(*p) p+=2;
  }
  return 0;
}

static long ws_read_frame(int fd, char *buf){
  u8 hdr[2];
  long n = read(fd, hdr, 2);
  if(n != 2) return -1;
  int opcode = hdr[0] & 0x0F;
  if(opcode == 8) return -2;
  int masked  = (hdr[1] >> 7) & 1;
  long plen   = hdr[1] & 0x7F;
  if(plen == 126){
    u8 ext[2]; if(read(fd,ext,2)!=2) return -1;
    plen = ((long)ext[0]<<8)|(long)ext[1];
  } else if(plen == 127){
    u8 ext[8]; if(read(fd,ext,8)!=8) return -1;
    plen = 0;
    int q;
    for(q=0;q<8;q++) plen=(plen<<8)|(long)ext[q];
  }
  u8 mask[4]={0,0,0,0};
  if(masked){ if(read(fd,mask,4)!=4) return -1; }
  long got=0;
  while(got<plen){
    long r=read(fd,(u8*)buf+got,plen-got);
    if(r<=0) return -1;
    got+=r;
  }
  if(masked){
    long q;
    for(q=0;q<plen;q++) buf[q]^=mask[q%4];
  }
  buf[plen]=0;
  return plen;
}

static int ws_write_text(int fd, const char *payload, long plen){
  u8 hdr[10]; int hlen=0;
  hdr[hlen++]=0x81;
  if(plen < 126){
    hdr[hlen++]=(u8)plen;
  } else if(plen < 65536){
    hdr[hlen++]=126;
    hdr[hlen++]=(u8)(plen>>8);
    hdr[hlen++]=(u8)(plen);
  } else {
    hdr[hlen++]=127;
    int q; for(q=7;q>=0;q--) hdr[hlen++]=(u8)(plen>>(q*8));
  }
  write(fd, hdr, hlen);
  write(fd, payload, plen);
  return 0;
}

typedef struct { int fd; } conn_arg;
static conn_arg g_args[1024];
static int g_arg_idx=0;

void *handle_ws_conn(void *arg){
  int fd = ((conn_arg*)arg)->fd;
  char req[8192];
  long n = read(fd, req, 8191);
  if(n <= 0){ close(fd); return 0; }
  req[n]=0;
  char ws_key[256]="";
  if(!find_header(req, "Sec-WebSocket-Key", ws_key, 256)){
    const char *msg =
      "HTTP/1.1 400 Bad Request\r\n"
      "Content-Type: text/plain\r\nContent-Length: 28\r\nConnection: close\r\n\r\n"
      "Expected WebSocket upgrade.\n";
    write(fd, msg, strlen(msg)); close(fd); return 0;
  }
  const char *magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  char combined[512];
  int klen=(int)strlen(ws_key), mlen=(int)strlen(magic);
  memcpy(combined, ws_key, klen);
  memcpy(combined+klen, magic, mlen);
  combined[klen+mlen]=0;
  u8 digest[20];
  sha1((const u8*)combined, (unsigned long)(klen+mlen), digest);
  char accept_b64[64];
  b64enc(digest, 20, accept_b64);
  char hdr[512];
  int hl = snprintf(hdr, 512,
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: %s\r\n\r\n",
    accept_b64);
  write(fd, hdr, hl);
  char payload[65537];
  for(;;){
    long plen = ws_read_frame(fd, payload);
    if(plen < 0) break;
    ws_write_text(fd, payload, plen);
  }
  close(fd);
  return 0;
}

static int g_ws_listen=-1;

const char *ws_serve(int port){
  if(g_ws_listen < 0){
    g_ws_listen = socket(2,1,0);
    int one=1; setsockopt(g_ws_listen,0xffff,0x0004,&one,4);
    struct sockaddr_in a;
    char *pp=(char*)&a; int k; for(k=0;k<16;k++) pp[k]=0;
    a.sin_len=16; a.sin_family=2;
    a.sin_port=(unsigned short)(((port&0xff)<<8)|((port>>8)&0xff));
    a.sin_addr.s_addr=0;
    if(bind(g_ws_listen,&a,16)<0){
      close(g_ws_listen); g_ws_listen=-1; return "BIND_FAIL";
    }
    listen(g_ws_listen, 64);
  }
  for(;;){
    int c = accept(g_ws_listen,0,0);
    if(c < 0) continue;
    int idx = g_arg_idx++ & 1023;
    g_args[idx].fd = c;
    pthread_t t;
    if(pthread_create(&t,0,handle_ws_conn,(void*)&g_args[idx])==0)
      pthread_detach(t);
    else
      close(c);
  }
  return "STOPPED";
}
',
  symbol := 'ws_serve', sql_name := 'ws_serve',
  return_type := 'varchar', arg_types := ['i32'],
  stability := 'volatile', library := 'c');

-- ws_serve(18097) blocks forever -- this statement IS the server.
-- Pipe to duckdb in background:
--   duckdb :memory: < serve_ws.sql &
SELECT ws_serve(18097) AS ws_server;
