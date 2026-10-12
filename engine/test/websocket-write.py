"""Verifica che due scritture WebSocket non intercalino i loro byte TLS."""
import os
from pathlib import Path
import subprocess
import tempfile

source = Path(os.environ.get('WA_ENGINE_SOURCE', 'engine/src/engine.c')).read_text()
start = source.index('static int ws_write_all(')
end = source.index('\nstatic int ws_send_frame', start)
function = source[start:end]
harness = r'''
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#define MBEDTLS_ERR_SSL_WANT_WRITE -1
#define MBEDTLS_ERR_SSL_WANT_READ -2
typedef struct { int th_on; pthread_mutex_t mu; int ssl; struct { int fd; } net; } WS;
static unsigned char wire[8192]; static size_t used;
// Cede la CPU al writer in attesa quando il mutex viene rilasciato.
static int fair_unlock(pthread_mutex_t *mu) { int r=pthread_mutex_unlock(mu); usleep(1000); return r; }
#define pthread_mutex_unlock fair_unlock
static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000LL+t.tv_nsec/1000000; }
static void elog(const char *fmt, ...) {}
static int mbedtls_ssl_write(int *ssl, const unsigned char *p, size_t n) {
  if(n>8) n=8; memcpy(wire+used,p,n); used+=n;
  usleep(1000); return (int)n;
}
FUNCTION
static WS ws; static pthread_barrier_t start_barrier;
static void *writer(void *arg) {
 unsigned char payload[4096]; memset(payload,*(char *)arg,sizeof payload);
 pthread_barrier_wait(&start_barrier);
 return (void *)(long)ws_write_all(&ws,payload,sizeof payload);
}
int main(void) {
 ws.th_on=1; pthread_mutex_init(&ws.mu,NULL); pthread_barrier_init(&start_barrier,NULL,2);
 pthread_t a,b; char x='A',y='B'; pthread_create(&a,NULL,writer,&x); pthread_create(&b,NULL,writer,&y);
 void *ra,*rb; pthread_join(a,&ra); pthread_join(b,&rb);
 int valid=used==8192 && !ra && !rb;
 for(size_t i=0;i<4096 && valid;i++) valid=wire[i]==wire[0] && wire[4096+i]!=wire[0] && wire[4096+i]==wire[4096];
 if(!valid) { fprintf(stderr,"ERRORE: byte di due frame intercalati\n"); return 1; }
 puts("OK: scritture concorrenti serializzate per l'intero frame"); return 0;
}
'''.replace('FUNCTION', function)
with tempfile.TemporaryDirectory(prefix='wa-write-test-') as td:
    c = Path(td)/'test.c'
    c.write_text(harness)
    executable = Path(td)/'test'
    subprocess.run(['cc','-O2','-pthread',str(c),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
