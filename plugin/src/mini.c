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

int memcmp(const void *a, const void *b, size_t n) { const unsigned char *p = a, *q = b; while (n--) { if (*p != *q) return *p - *q; p++; q++; } return 0; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
int inet_pton(int af, const char *src, void *dst) { (void)af; (void)src; (void)dst; return 0; }

// memoria: un unico blocco (heap_init) gestito con una lista di blocchi; free() libera davvero e unisce i blocchi vicini.
typedef struct Blk { size_t size; size_t used; } Blk;   // size = byte utili dopo l'intestazione
static unsigned char *heap_base; static size_t heap_size;
void heap_init(void *base, size_t size) { heap_base = (unsigned char *)(((size_t)base + 7) & ~(size_t)7); heap_size = (size - (size_t)(heap_base - (unsigned char *)base)) & ~(size_t)7; Blk *b = (Blk *)heap_base; b->size = heap_size - sizeof(Blk); b->used = 0; }
static Blk *nextb(Blk *b) { unsigned char *n = (unsigned char *)b + sizeof(Blk) + b->size; return n >= heap_base + heap_size ? NULL : (Blk *)n; }
size_t heap_free_bytes(void) { size_t t = 0; if (!heap_base) return 0; for (Blk *b = (Blk *)heap_base; b; b = nextb(b)) if (!b->used) t += b->size; return t; }
void *malloc(size_t n) {
  if (!heap_base) return NULL; n = (n + 7) & ~(size_t)7; if (!n) n = 8;
  for (Blk *b = (Blk *)heap_base; b; b = nextb(b)) {
    if (b->used) continue;
    Blk *x; while ((x = nextb(b)) && !x->used) b->size += sizeof(Blk) + x->size;   // unisce i liberi adiacenti
    if (b->size < n) continue;
    if (b->size >= n + sizeof(Blk) + 16) { Blk *r = (Blk *)((unsigned char *)b + sizeof(Blk) + n); r->size = b->size - n - sizeof(Blk); r->used = 0; b->size = n; }
    b->used = 1; return (unsigned char *)b + sizeof(Blk);
  }
  return NULL;
}
void free(void *p) { if (p) ((Blk *)((unsigned char *)p - sizeof(Blk)))->used = 0; }
void *calloc(size_t a, size_t b) { void *p = malloc(a * b); if (p) memset(p, 0, a * b); return p; }
void *realloc(void *p, size_t n) {
  if (!p) return malloc(n);
  size_t old = ((Blk *)((unsigned char *)p - sizeof(Blk)))->size; if (n <= old) return p;
  void *q = malloc(n); if (q) { memcpy(q, p, old); free(p); } return q;
}
void *wa_calloc(size_t a, size_t b) { return calloc(a, b); }
void wa_free(void *p) { free(p); }
void arena_reset(void) {}
