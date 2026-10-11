#include "lite.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static void on_node(const LiteNode *n, void *ud) {
  printf("  nodo <%s> da=%s part=%s notify=%s tipo=%s id=%s\n", n->tag, n->from, n->participant, n->notify, n->type, n->id); fflush(stdout);
  if (!strcmp(n->tag, "message")) printf("  >>> NUOVO MESSAGGIO da %s (%s)\n", n->notify[0] ? n->notify : n->from, n->participant[0] ? n->participant : n->from);
}
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "uso: %s lite.bin cacert.pem [secondi]\n", argv[0]); return 2; }
  FILE *f = fopen(argv[1], "rb"); if (!f) { perror("lite.bin"); return 1; } uint8_t b[4096]; size_t n = fread(b, 1, sizeof b, f); fclose(f);
  if (n < 70 || memcmp(b, "WALT", 4)) { fprintf(stderr, "file non valido\n"); return 1; } size_t pl = b[4 + 64] | (b[4 + 65] << 8);
  LiteClient *c = lite_new(b + 4, b + 36, b + 70, pl, argv[2]); int secs = argc > 3 ? atoi(argv[3]) : 60;
  int rc = lite_connect(c); printf("connect: %d %s\n", rc, lite_last_error(c)); if (rc != 0) return 1;
  for (int i = 0; i < secs * 10; i++) { rc = lite_poll(c, 100, on_node, NULL); if (rc != 0) { printf("poll: %d %s\n", rc, lite_last_error(c)); break; } }
  lite_close(c); return 0;
}
