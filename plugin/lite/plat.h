// Strato di piattaforma per il client leggero dentro il plugin (e per provarlo sul PC con la stessa configurazione di mbedTLS).
// Implementazioni: plat_pc.c (prove sul PC) e plugin/src/lite_vita.c (plugin della shell).
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { int fd; } mbedtls_net_context;
void mbedtls_net_init(mbedtls_net_context *c);
void mbedtls_net_free(mbedtls_net_context *c);
int mbedtls_net_connect(mbedtls_net_context *c, const char *host, const char *port, int proto);   // si collega agli indirizzi impostati con plat_set_ips
int mbedtls_net_set_nonblock(mbedtls_net_context *c);
int mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len);
int mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len);
int mbedtls_net_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout);
#define MBEDTLS_NET_PROTO_TCP 0
#include "mbedtls/entropy.h"   // solo le dichiarazioni: le funzioni sono nostre (plat_*.c), senza il modulo entropia di mbedTLS
void plat_set_seed(const uint8_t *seed, size_t n);          // seme casuale preparato dall'app
void plat_set_ips(const uint32_t *ips_be, int n);            // indirizzi IPv4 (ordine di rete) di web.whatsapp.com
long long plat_now_ms(void);
void plat_wait_fd(int fd, int usec, int want_read, int want_write);
void plat_sleep_ms(int ms);
