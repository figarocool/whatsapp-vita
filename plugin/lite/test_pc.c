// Prova sul PC del client leggero con la configurazione ridotta di mbedTLS del plugin: test_pc lite.bin [secondi]
#include "../../engine/lite/lite.h"
#include "plat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <arpa/inet.h>
static void on_node(const LiteNode *n, void *ud) {
  (void)ud; printf("  nodo <%s> da=%s part=%s notify=%s tipo=%s id=%s\n", n->tag, n->from, n->participant, n->notify, n->type, n->id); fflush(stdout);
  if (!strcmp(n->tag, "message")) printf("  >>> NUOVO MESSAGGIO da %s (%s)\n", n->notify[0] ? n->notify : n->from, n->participant[0] ? n->participant : n->from);
}
int main(int argc, char **argv) {
  if (argc < 2) return 2; FILE *f = fopen(argv[1], "rb"); if (!f) { perror("lite.bin"); return 1; } uint8_t b[4096]; size_t n = fread(b, 1, sizeof b, f); fclose(f);
  if (n < 72 || memcmp(b, "WAL2", 4)) { fprintf(stderr, "file non valido\n"); return 1; } size_t pl = b[68] | (b[69] << 8); size_t o = 70 + pl; int nip = b[o++]; uint32_t ips[8]; for (int i = 0; i < nip; i++) { memcpy(&ips[i], b + o, 4); o += 4; } plat_set_ips(ips, nip); plat_set_seed(b + o, 32); o += 32; int ml = b[o++]; printf("utente %.*s, %d indirizzi\n", ml, b + o, nip);
  LiteClient *c = lite_new(b + 4, b + 36, b + 70, pl, NULL); int secs = argc > 2 ? atoi(argv[2]) : 40;
  long long t0 = plat_now_ms(); int rc = lite_connect(c); printf("connect: %d %s (%lld ms)\n", rc, lite_last_error(c), plat_now_ms() - t0); if (rc != 0) return 1;
  for (int i = 0; i < secs * 10; i++) { rc = lite_poll(c, 100, on_node, NULL); if (rc != 0) { printf("poll: %d %s\n", rc, lite_last_error(c)); break; } }
  lite_close(c); return 0;
}
