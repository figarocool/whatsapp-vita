// Mini libreria standard per il plugin della shell.
// Il plugin parte presto all'avvio, quando SceLibc non e' ancora caricata: qualsiasi import da li' fa rifiutare il modulo.
// Qui ci sono solo le poche funzioni che servono, scritte a mano o appoggiate a SceLibKernel (sceClib*), che e' sempre presente.
#include <psp2/kernel/clib.h>
#include <stdarg.h>
#include <stddef.h>

void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n) {
  unsigned char *p = d; const unsigned char *q = s;
  if (p < q) { while (n--) *p++ = *q++; } else { p += n; q += n; while (n--) *--p = *--q; }
  return d;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? (unsigned char)*a - (unsigned char)*b : 0; }
char *strstr(const char *h, const char *n) {
  if (!*n) return (char *)h;
  for (; *h; h++) { const char *a = h, *b = n; while (*a && *b && *a == *b) { a++; b++; } if (!*b) return (char *)h; }
  return NULL;
}
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char *)s; return c == 0 ? (char *)s : NULL; }
int atoi(const char *s) { int v = 0, neg = 0; while (*s == ' ') s++; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return neg ? -v : v; }
int snprintf(char *d, size_t n, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); int r = sceClibVsnprintf(d, n, fmt, ap); va_end(ap); return r;
}
double strtod(const char *s, char **end) {
  const char *p = s; int neg = 0; double v = 0;
  if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  if (*p == '.') { p++; double f = 0.1; while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * f; f *= 0.1; } }
  if (*p == 'e' || *p == 'E') {
    const char *q = p + 1; int en = 0, ex = 0; if (*q == '-') { en = 1; q++; } else if (*q == '+') q++;
    if (*q >= '0' && *q <= '9') { while (*q >= '0' && *q <= '9') ex = ex * 10 + (*q++ - '0'); while (ex-- > 0) v = en ? v / 10 : v * 10; p = q; }
  }
  if (end) *end = (char *)p;
  return neg ? -v : v;
}

// memoria: arena statica. free() non libera niente; il plugin azzera l'arena (arena_reset) a ogni giro, dopo aver liberato il JSON.
#define ARENA_SZ (96 * 1024)
static unsigned char arena[ARENA_SZ] __attribute__((aligned(8)));
static size_t arena_used;
void arena_reset(void) { arena_used = 0; }
void *malloc(size_t n) {
  n = (n + 7) & ~(size_t)7; if (arena_used + n + 8 > ARENA_SZ) return NULL;
  size_t *h = (size_t *)(arena + arena_used); *h = n; arena_used += n + 8; return (unsigned char *)h + 8;
}
void *calloc(size_t a, size_t b) { void *p = malloc(a * b); if (p) memset(p, 0, a * b); return p; }
void free(void *p) { (void)p; }
void *realloc(void *p, size_t n) {
  if (!p) return malloc(n);
  size_t old = *(size_t *)((unsigned char *)p - 8); if (n <= old) return p;
  void *q = malloc(n); if (q) memcpy(q, p, old); return q;
}
