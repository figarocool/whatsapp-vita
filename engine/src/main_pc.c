#include "engine.h"
#include <stdio.h>
#include <string.h>
static void logline(const char *l) { printf("%s\n", l); fflush(stdout); }
int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "uso: %s cacert.pem datadir modulo.js\n", argv[0]); return 2; }
  Engine *e = eng_new(argv[1], argv[2], logline);
  if (eng_run_module(e, argv[3]) != 0) return 1;
  while (eng_loop_step(e, 100) > 0) {}
  eng_free(e); return 0;
}
