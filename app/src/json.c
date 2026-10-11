#include "json.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p; } P;
static void ws(P *p) { while (*p->p == ' ' || *p->p == '\n' || *p->p == '\r' || *p->p == '\t') p->p++; }
static jv *val(P *p);

static void put_utf8(char **o, unsigned c) {
  if (c < 0x80) *(*o)++ = c;
  else if (c < 0x800) { *(*o)++ = 0xC0 | (c >> 6); *(*o)++ = 0x80 | (c & 63); }
  else if (c < 0x10000) { *(*o)++ = 0xE0 | (c >> 12); *(*o)++ = 0x80 | ((c >> 6) & 63); *(*o)++ = 0x80 | (c & 63); }
  else { *(*o)++ = 0xF0 | (c >> 18); *(*o)++ = 0x80 | ((c >> 12) & 63); *(*o)++ = 0x80 | ((c >> 6) & 63); *(*o)++ = 0x80 | (c & 63); }
}
static unsigned hex4(const char *s) { unsigned v = 0; for (int i = 0; i < 4; i++) { char c = s[i]; if (!c) return 0xFFFD; v <<= 4; v |= c <= '9' ? c - '0' : (c | 32) - 'a' + 10; } return v; }

static char *str(P *p) {
  p->p++; // "
  size_t cap = 16, n = 0; const char *q = p->p;
  while (*q && *q != '"') { if (*q == '\\' && q[1]) q++; q++; }
  cap = (q - p->p) + 8;
  char *out = malloc(cap), *o = out;
  while (*p->p && *p->p != '"') {
    if (*p->p == '\\') {
      p->p++;
      switch (*p->p) {
        case 'n': *o++ = '\n'; break; case 't': *o++ = '\t'; break; case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break; case 'f': *o++ = '\f'; break;
        case 'u': {
          unsigned c = hex4(p->p + 1); for (int k = 0; k < 4 && p->p[1]; k++) p->p++;
          if (c >= 0xD800 && c < 0xDC00 && p->p[1] == '\\' && p->p[2] == 'u') {
            unsigned d = hex4(p->p + 3); for (int k = 0; k < 6 && p->p[1]; k++) p->p++;
            c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
          }
          put_utf8(&o, c); break;
        }
        default: *o++ = *p->p;
      }
      if (*p->p) p->p++;
    } else *o++ = *p->p++;
  }
  if (*p->p == '"') p->p++;
  *o = 0; (void)n;
  return out;
}

static jv *mk(jtype t) { jv *v = calloc(1, sizeof *v); v->t = t; return v; }
static void push(jv *a, jv *it, char *key) {
  a->items = realloc(a->items, sizeof(jv *) * (a->count + 1));
  if (a->t == J_OBJ) { a->keys = realloc(a->keys, sizeof(char *) * (a->count + 1)); a->keys[a->count] = key; }
  a->items[a->count++] = it;
}

static jv *val(P *p) {
  ws(p);
  char c = *p->p;
  if (c == '{') {
    jv *o = mk(J_OBJ); p->p++; ws(p);
    if (*p->p == '}') { p->p++; return o; }
    while (*p->p) {
      ws(p); if (*p->p != '"') break;
      char *k = str(p); ws(p); if (*p->p == ':') p->p++;
      jv *v = val(p); if (!v) { free(k); break; }
      push(o, v, k); ws(p);
      if (*p->p == ',') { p->p++; continue; }
      if (*p->p == '}') p->p++;
      break;
    }
    return o;
  }
  if (c == '[') {
    jv *a = mk(J_ARR); p->p++; ws(p);
    if (*p->p == ']') { p->p++; return a; }
    while (*p->p) {
      jv *v = val(p); if (!v) break;
      push(a, v, NULL); ws(p);
      if (*p->p == ',') { p->p++; continue; }
      if (*p->p == ']') p->p++;
      break;
    }
    return a;
  }
  if (c == '"') { jv *v = mk(J_STR); v->s = str(p); return v; }
  if (!strncmp(p->p, "true", 4)) { p->p += 4; jv *v = mk(J_BOOL); v->n = 1; return v; }
  if (!strncmp(p->p, "false", 5)) { p->p += 5; return mk(J_BOOL); }
  if (!strncmp(p->p, "null", 4)) { p->p += 4; return mk(J_NULL); }
  if (c == '-' || (c >= '0' && c <= '9')) { char *e; jv *v = mk(J_NUM); v->n = strtod(p->p, &e); p->p = e; return v; }
  return NULL;
}

jv *json_parse(const char *src) { P p = { src }; return val(&p); }
void json_free(jv *v) {
  if (!v) return;
  for (int i = 0; i < v->count; i++) { json_free(v->items[i]); if (v->keys) free(v->keys[i]); }
  free(v->items); free(v->keys); free(v->s); free(v);
}
jv *json_get(const jv *o, const char *key) {
  if (!o || o->t != J_OBJ) return NULL;
  for (int i = 0; i < o->count; i++) if (!strcmp(o->keys[i], key)) return o->items[i];
  return NULL;
}
const char *json_str(const jv *o, const char *key, const char *def) { jv *v = json_get(o, key); return v && v->t == J_STR ? v->s : def; }
double json_num(const jv *o, const char *key, double def) { jv *v = json_get(o, key); return v && v->t == J_NUM ? v->n : def; }
int json_bool(const jv *o, const char *key, int def) { jv *v = json_get(o, key); return v && v->t == J_BOOL ? (int)v->n : def; }
