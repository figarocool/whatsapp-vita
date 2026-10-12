// Piattaforma PC per provare lite.c con la configurazione ridotta di mbedTLS (stessi file del plugin, ma con socket POSIX).
#include "plat.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
void *wa_calloc(size_t n, size_t s) { return calloc(n, s); }
void wa_free(void *p) { free(p); }
static uint32_t ips[8]; static int nips;
void plat_set_ips(const uint32_t *v, int n) { nips = n > 8 ? 8 : n; memcpy(ips, v, nips * 4); }
static uint8_t seed[64]; static size_t seedn; static unsigned ctr;
void plat_set_seed(const uint8_t *s, size_t n) { seedn = n > 64 ? 64 : n; memcpy(seed, s, seedn); }
long long plat_now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
void plat_sleep_ms(int ms) { usleep(ms * 1000); }
void plat_wait_fd(int fd, int usec, int rd, int wr) { fd_set f; FD_ZERO(&f); FD_SET(fd, &f); struct timeval tv = { 0, usec }; select(fd + 1, rd ? &f : NULL, wr ? &f : NULL, NULL, &tv); }
void mbedtls_net_init(mbedtls_net_context *c) { c->fd = -1; }
void mbedtls_net_free(mbedtls_net_context *c) { if (c->fd >= 0) close(c->fd); c->fd = -1; }
int mbedtls_net_connect(mbedtls_net_context *c, const char *host, const char *port, int proto) {
  (void)host; (void)proto; for (int i = 0; i < nips; i++) { int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) continue; struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons((unsigned short)atoi(port)); a.sin_addr.s_addr = ips[i];
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) { c->fd = s; return 0; } close(s); } return -1;
}
int mbedtls_net_set_nonblock(mbedtls_net_context *c) { return fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL) | O_NONBLOCK); }
int mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len) { int fd = ((mbedtls_net_context *)ctx)->fd; int r = (int)send(fd, buf, len, 0); if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : -0x0046; return r; }
int mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len) { int fd = ((mbedtls_net_context *)ctx)->fd; int r = (int)recv(fd, buf, len, 0); if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ : -0x004C; if (r == 0) return -0x004E; return r; }
int mbedtls_net_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout) { int fd = ((mbedtls_net_context *)ctx)->fd; fd_set f; FD_ZERO(&f); FD_SET(fd, &f); struct timeval tv = { timeout / 1000, (timeout % 1000) * 1000 }; int r = select(fd + 1, &f, NULL, NULL, &tv); if (r == 0) return MBEDTLS_ERR_SSL_TIMEOUT; if (r < 0) return -0x004C; return mbedtls_net_recv(ctx, buf, len); }
void mbedtls_entropy_init(mbedtls_entropy_context *e) { (void)e; }
void mbedtls_entropy_free(mbedtls_entropy_context *e) { (void)e; }
// entropia: SHA-256( seme dell'app || contatore || tempo ), in modalita' contatore
int mbedtls_entropy_func(void *data, unsigned char *out, size_t len) {
  (void)data; size_t off = 0; while (off < len) { uint8_t h[32]; mbedtls_sha256_context s; mbedtls_sha256_init(&s); mbedtls_sha256_starts(&s, 0); mbedtls_sha256_update(&s, seed, seedn); unsigned c = ++ctr; long long t = plat_now_ms(); mbedtls_sha256_update(&s, (uint8_t *)&c, 4); mbedtls_sha256_update(&s, (uint8_t *)&t, 8); mbedtls_sha256_finish(&s, h); mbedtls_sha256_free(&s); size_t k = len - off < 32 ? len - off : 32; memcpy(out + off, h, k); off += k; } return 0;
}
