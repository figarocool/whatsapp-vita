#include "engine.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static void logline(const char *l) { printf("%s\n", l); fflush(stdout); }
int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "uso: %s cacert.pem datadir modulo.js [porta-tcp]\n", argv[0]); return 2; }
  Engine *e = eng_new(argv[1], argv[2], logline);
  if (argc > 4) { int port = atoi(argv[4]); if (eng_listen_tcp(e, port) != 0) { fprintf(stderr, "porta %d non disponibile\n", port); return 1; } printf("in ascolto su 127.0.0.1:%d\n", port); fflush(stdout); }
  if (eng_run_module(e, argv[3]) != 0) return 1;
  while (eng_loop_step(e, 100) > 0) {}
  eng_free(e); return 0;
}
