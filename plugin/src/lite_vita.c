// Piattaforma Vita per il client leggero dentro il plugin della shell (vedi plugin/lite/plat.h).
// Le funzioni di rete NON sono collegate in modo fisso: la shell rifiuta i plugin che importano librerie non ancora caricate,
// quindi si cercano a runtime tra le esportazioni dei moduli SceNet*.
#include "../lite/plat.h"
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <taihen.h>
#include <string.h>
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"

typedef int (*f_init)(SceNetInitParam *); typedef int (*f_socket)(const char *, int, int, int);
typedef int (*f_setopt)(int, int, int, const void *, unsigned); typedef int (*f_connect)(int, const SceNetSockaddr *, unsigned);
typedef int (*f_send)(int, const void *, unsigned, int); typedef int (*f_recv)(int, void *, unsigned, int); typedef int (*f_close)(int);
static f_init n_init; static f_socket n_socket; static f_setopt n_setopt; static f_connect n_connect; static f_send n_send; static f_recv n_recv; static f_close n_close;
static int net_ready;
void plog_s(const char *s);

static uintptr_t find_net_fn(unsigned nid) {
  static SceUID ids[256]; static SceKernelModuleInfo mi; SceSize cnt = 256;
  if (sceKernelGetModuleList(0xFF, ids, &cnt) < 0) return 0;
  for (SceSize i = 0; i < cnt; i++) {
    mi.size = sizeof mi; if (sceKernelGetModuleInfo(ids[i], &mi) < 0) continue;
    if (strncmp(mi.module_name, "SceNet", 6) != 0) continue;
    uintptr_t fn = 0; if (taiGetModuleExportFunc(mi.module_name, TAI_ANY_LIBRARY, nid, &fn) >= 0 && fn) return fn;
  }
  return 0;
}
int lv_net_resolve(void) {
  if (net_ready) return 1;
  static int loaded; if (!loaded) { loaded = 1; sceSysmoduleLoadModule(SCE_SYSMODULE_NET); }
  static const unsigned nids[7] = { 0xEB03E265u, 0xF084FCE3u, 0x065505CAu, 0x11E5B6F6u, 0xE3DD8CD9u, 0x023643B7u, 0x29822B4Du };
  uintptr_t *dst[7] = { (uintptr_t *)&n_init, (uintptr_t *)&n_socket, (uintptr_t *)&n_setopt, (uintptr_t *)&n_connect, (uintptr_t *)&n_send, (uintptr_t *)&n_recv, (uintptr_t *)&n_close };
  for (int i = 0; i < 7; i++) { uintptr_t fn = find_net_fn(nids[i]); if (!fn) return 0; *dst[i] = fn; }
  net_ready = 1; return 1;
}
int lv_net_init(void) { static char pool[64 * 1024]; if (!lv_net_resolve()) return -1; SceNetInitParam ip = { pool, sizeof pool, 0 }; return n_init(&ip); }   // "gia' inizializzato" e' normale

static uint32_t ips[8]; static int nips;
void plat_set_ips(const uint32_t *v, int n) { nips = n > 8 ? 8 : n; memcpy(ips, v, nips * 4); }
static uint8_t seed[64]; static size_t seedn; static unsigned ctr;
void plat_set_seed(const uint8_t *s, size_t n) { seedn = n > 64 ? 64 : n; memcpy(seed, s, seedn); }
long long plat_now_ms(void) { return (long long)(sceKernelGetProcessTimeWide() / 1000); }
void plat_sleep_ms(int ms) { sceKernelDelayThread(ms * 1000); }
void plat_wait_fd(int fd, int usec, int rd, int wr) { (void)fd; (void)rd; (void)wr; sceKernelDelayThread(usec > 50000 ? 50000 : usec); }   // senza select: si controlla il socket a intervalli brevi

void mbedtls_net_init(mbedtls_net_context *c) { c->fd = -1; }
void mbedtls_net_free(mbedtls_net_context *c) { if (c->fd >= 0 && net_ready) n_close(c->fd); c->fd = -1; }
int mbedtls_net_connect(mbedtls_net_context *c, const char *host, const char *port, int proto) {
  (void)host; (void)proto; if (!lv_net_resolve()) return -1; int p = 0; while (*port) p = p * 10 + (*port++ - '0');
  for (int i = 0; i < nips; i++) {
    int s = n_socket("walite", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0); if (s < 0) continue;
    int to = 10 * 1000 * 1000; n_setopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &to, sizeof to); n_setopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &to, sizeof to);
    SceNetSockaddrIn a; memset(&a, 0, sizeof a); a.sin_len = sizeof a; a.sin_family = SCE_NET_AF_INET; a.sin_port = (unsigned short)(((p & 0xFF) << 8) | ((p >> 8) & 0xFF)); a.sin_addr.s_addr = ips[i];
    if (n_connect(s, (SceNetSockaddr *)&a, sizeof a) >= 0) { c->fd = s; return 0; } n_close(s);
  }
  return -1;
}
int mbedtls_net_set_nonblock(mbedtls_net_context *c) { int one = 1; return n_setopt(c->fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof one); }
static int again(int r) { return r == (int)SCE_NET_ERROR_EAGAIN || r == (int)SCE_NET_ERROR_EWOULDBLOCK || r == (int)SCE_NET_ERROR_ETIMEDOUT || r == (int)SCE_NET_ERROR_EINTR; }
int mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len) { int r = n_send(((mbedtls_net_context *)ctx)->fd, buf, (unsigned)len, 0); if (r < 0) return again(r) ? MBEDTLS_ERR_SSL_WANT_WRITE : -0x0046; return r; }
int mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len) { int r = n_recv(((mbedtls_net_context *)ctx)->fd, buf, (unsigned)len, 0); if (r < 0) return again(r) ? MBEDTLS_ERR_SSL_WANT_READ : -0x004C; if (r == 0) return -0x004E; return r; }
int mbedtls_net_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout) {   // a socket bloccante con RCVTIMEO da 10 s (impostato alla connessione)
  (void)timeout; int r = n_recv(((mbedtls_net_context *)ctx)->fd, buf, (unsigned)len, 0); if (r < 0) return again(r) ? MBEDTLS_ERR_SSL_TIMEOUT : -0x004C; if (r == 0) return -0x004E; return r;
}
void mbedtls_entropy_init(mbedtls_entropy_context *e) { (void)e; }
void mbedtls_entropy_free(mbedtls_entropy_context *e) { (void)e; }
// entropia: SHA-256( seme dell'app || contatore || tempi ) in modalita' contatore; il seme e' casuale (lo prepara il motore dell'app)
int mbedtls_entropy_func(void *data, unsigned char *out, size_t len) {
  (void)data; size_t off = 0;
  while (off < len) {
    uint8_t h[32]; mbedtls_sha256_context s; mbedtls_sha256_init(&s); mbedtls_sha256_starts(&s, 0); mbedtls_sha256_update(&s, seed, seedn);
    unsigned c = ++ctr; unsigned long long t = sceKernelGetProcessTimeWide(), u = sceKernelGetSystemTimeWide(); mbedtls_sha256_update(&s, (uint8_t *)&c, 4); mbedtls_sha256_update(&s, (uint8_t *)&t, 8); mbedtls_sha256_update(&s, (uint8_t *)&u, 8);
    mbedtls_sha256_finish(&s, h); mbedtls_sha256_free(&s); size_t k = len - off < 32 ? len - off : 32; memcpy(out + off, h, k); off += k;
  }
  return 0;
}
