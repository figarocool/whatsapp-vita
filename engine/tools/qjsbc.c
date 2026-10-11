// Compila un modulo JavaScript in bytecode QuickJS (.qbc): sulla Vita si carica molto piu' in fretta che analizzando il testo.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "uso: %s ingresso.js uscita.qbc\n", argv[0]); return 2; }
  FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; } fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *src = malloc(n + 1); fread(src, 1, n, f); fclose(f); src[n] = 0;
  JSRuntime *rt = JS_NewRuntime(); JS_SetStripInfo(rt, JS_STRIP_DEBUG); JSContext *ctx = JS_NewContext(rt);
  JSValue v = JS_Eval(ctx, src, n, argv[1], JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(v)) { JSValue ex = JS_GetException(ctx); const char *s = JS_ToCString(ctx, ex); fprintf(stderr, "errore di compilazione: %s\n", s); return 1; }
  size_t outn; uint8_t *out = JS_WriteObject(ctx, &outn, v, JS_WRITE_OBJ_BYTECODE);
  if (!out) { fprintf(stderr, "scrittura del bytecode fallita\n"); return 1; }
  FILE *o = fopen(argv[2], "wb"); fwrite(out, 1, outn, o); fclose(o); printf("bytecode: %zu byte (sorgente %ld byte)\n", outn, n); return 0;
}
