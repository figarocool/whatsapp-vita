#include "engine.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <dirent.h>
#include <zlib.h>
#include "mbedtls/md.h"
#include "mbedtls/cipher.h"
#include "mbedtls/ssl.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"

#define MAXWS 4
#define MAXH 64

typedef struct Timer { struct Timer *next; int id; long long due; long long every; JSValue fn; } Timer;
typedef struct { mbedtls_md_context_t md; int hmac, used; } HHandle;
typedef struct { mbedtls_cipher_context_t c; int used, gcm, enc; unsigned char tag[16]; int has_tag; } CHandle;
typedef struct {
  int used, fd, state;              // state: 0 libero, 1 aperto, 2 chiuso da segnalare
  mbedtls_ssl_context ssl; mbedtls_ssl_config conf; mbedtls_net_context net;
  unsigned char *rx; size_t rxlen, rxcap;
  unsigned char *frag; size_t fraglen; int fragop;
  JSValue cb;                      // funzione (evento, dati)
  int closed_notified;
} WS;

struct Engine {
  JSRuntime *rt; JSContext *ctx;
  Timer *timers; int next_timer; EngineLog log;
  char cacert[400], datadir[400];
  mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context drbg; mbedtls_x509_crt ca; int ca_ok;
  HHandle hh[MAXH]; CHandle ch[MAXH]; WS ws[MAXWS];
};
static Engine *G;

static long long now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static void elog(const char *fmt, ...) { char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); if (G && G->log) G->log(b); else fprintf(stderr, "%s\n", b); }

// ---------- utilita' ----------
static uint8_t *get_bytes(JSContext *ctx, JSValueConst v, size_t *len) {
  size_t off, blen, bpe; uint8_t *p = JS_GetArrayBuffer(ctx, len, v);
  if (p) return p;
  JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &bpe);
  if (JS_IsException(ab)) { JS_FreeValue(ctx, JS_GetException(ctx)); return NULL; }
  size_t total; uint8_t *base = JS_GetArrayBuffer(ctx, &total, ab); JS_FreeValue(ctx, ab);
  if (!base) return NULL; *len = blen; return base + off;
}
static JSValue new_ab(JSContext *ctx, const void *p, size_t n) { return JS_NewArrayBufferCopy(ctx, (const uint8_t *)(p ? p : ""), n); }
#define NEED_BYTES(var, len, idx) size_t len = 0; uint8_t *var = get_bytes(ctx, argv[idx], &len); if (!var && !(JS_IsObject(argv[idx]))) return JS_ThrowTypeError(ctx, "serve un buffer")

// ---------- console ----------
static JSValue js_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  char line[4096]; size_t n = 0; line[0] = 0;
  for (int i = 0; i < argc && n < sizeof line - 2; i++) {
    const char *s = JS_ToCString(ctx, argv[i]); if (!s) continue;
    n += snprintf(line + n, sizeof line - n, "%s%s", i ? " " : "", s); JS_FreeCString(ctx, s);
  }
  elog("%s", line); return JS_UNDEFINED;
}

// ---------- timer ----------
static JSValue js_set_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "funzione richiesta");
  double ms = 0; if (argc > 1) JS_ToFloat64(ctx, &ms, argv[1]); if (ms < 0 || ms != ms) ms = 0;
  Timer *t = calloc(1, sizeof *t); t->id = ++G->next_timer; t->due = now_ms() + (long long)ms; t->every = magic ? (ms < 1 ? 1 : (long long)ms) : 0; t->fn = JS_DupValue(ctx, argv[0]);
  Timer **pp = &G->timers; while (*pp) pp = &(*pp)->next; *pp = t;
  return JS_NewInt32(ctx, t->id);
}
static JSValue js_clear_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int id = 0; if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
  for (Timer **pp = &G->timers; *pp; pp = &(*pp)->next) if ((*pp)->id == id) { Timer *t = *pp; *pp = t->next; JS_FreeValue(ctx, t->fn); free(t); break; }
  return JS_UNDEFINED;
}
static JSValue js_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { struct timeval tv; gettimeofday(&tv, NULL); return JS_NewFloat64(ctx, (double)tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0); }
static JSValue js_mono(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return JS_NewFloat64(ctx, (double)now_ms()); }

// ---------- casuali ----------
static JSValue js_random(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int n = 0; JS_ToInt32(ctx, &n, argv[0]); if (n < 0 || n > (1 << 24)) return JS_ThrowRangeError(ctx, "lunghezza");
  unsigned char *b = malloc(n ? n : 1); mbedtls_ctr_drbg_random(&G->drbg, b, n); JSValue r = new_ab(ctx, b, n); free(b); return r;
}

// ---------- hash / hmac ----------
static mbedtls_md_type_t md_by_name(const char *s) {
  if (!strcmp(s, "sha256")) return MBEDTLS_MD_SHA256; if (!strcmp(s, "sha512")) return MBEDTLS_MD_SHA512;
  if (!strcmp(s, "sha1")) return MBEDTLS_MD_SHA1; if (!strcmp(s, "md5")) return MBEDTLS_MD_MD5; if (!strcmp(s, "sha384")) return MBEDTLS_MD_SHA384;
  return MBEDTLS_MD_NONE;
}
// hash_new(alg, keyOrUndefined) -> handle
static JSValue js_hash_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *alg = JS_ToCString(ctx, argv[0]); if (!alg) return JS_EXCEPTION; mbedtls_md_type_t t = md_by_name(alg); JS_FreeCString(ctx, alg);
  if (t == MBEDTLS_MD_NONE) return JS_ThrowTypeError(ctx, "algoritmo hash sconosciuto");
  int h = -1; for (int i = 0; i < MAXH; i++) if (!G->hh[i].used) { h = i; break; } if (h < 0) return JS_ThrowInternalError(ctx, "troppi hash aperti");
  HHandle *x = &G->hh[h]; mbedtls_md_init(&x->md); x->used = 1; x->hmac = argc > 1 && !JS_IsUndefined(argv[1]);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(t);
  if (mbedtls_md_setup(&x->md, info, x->hmac) != 0) { mbedtls_md_free(&x->md); x->used = 0; return JS_ThrowInternalError(ctx, "md_setup"); }
  if (x->hmac) { size_t kl = 0; uint8_t *k = get_bytes(ctx, argv[1], &kl); mbedtls_md_hmac_starts(&x->md, k, kl); } else mbedtls_md_starts(&x->md);
  return JS_NewInt32(ctx, h);
}
static JSValue js_hash_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int h; JS_ToInt32(ctx, &h, argv[0]); if (h < 0 || h >= MAXH || !G->hh[h].used) return JS_ThrowTypeError(ctx, "hash non valido");
  NEED_BYTES(p, n, 1); HHandle *x = &G->hh[h]; if (x->hmac) mbedtls_md_hmac_update(&x->md, p, n); else mbedtls_md_update(&x->md, p, n); return JS_UNDEFINED;
}
static JSValue js_hash_final(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int h; JS_ToInt32(ctx, &h, argv[0]); if (h < 0 || h >= MAXH || !G->hh[h].used) return JS_ThrowTypeError(ctx, "hash non valido");
  HHandle *x = &G->hh[h]; unsigned char out[64]; if (x->hmac) mbedtls_md_hmac_finish(&x->md, out); else mbedtls_md_finish(&x->md, out);
  size_t n = mbedtls_md_get_size(mbedtls_md_info_from_ctx(&x->md)); mbedtls_md_free(&x->md); x->used = 0; return new_ab(ctx, out, n);
}

// ---------- cifrari AES (CBC con PKCS7, CTR, GCM) ----------
// cipher_new(mode 'cbc'|'ctr'|'gcm', encrypt(bool), key, iv) -> handle
static JSValue js_cipher_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *mode = JS_ToCString(ctx, argv[0]); if (!mode) return JS_EXCEPTION; int enc = JS_ToBool(ctx, argv[1]);
  size_t kl = 0, il = 0; uint8_t *key = get_bytes(ctx, argv[2], &kl), *iv = get_bytes(ctx, argv[3], &il);
  if (!key || !iv || (kl != 32 && kl != 16)) { JS_FreeCString(ctx, mode); return JS_ThrowTypeError(ctx, "chiave/iv non validi"); }
  mbedtls_cipher_type_t ct = MBEDTLS_CIPHER_NONE; int gcm = 0;
  if (!strcmp(mode, "cbc")) ct = kl == 32 ? MBEDTLS_CIPHER_AES_256_CBC : MBEDTLS_CIPHER_AES_128_CBC;
  else if (!strcmp(mode, "ctr")) ct = kl == 32 ? MBEDTLS_CIPHER_AES_256_CTR : MBEDTLS_CIPHER_AES_128_CTR;
  else if (!strcmp(mode, "gcm")) { ct = kl == 32 ? MBEDTLS_CIPHER_AES_256_GCM : MBEDTLS_CIPHER_AES_128_GCM; gcm = 1; }
  JS_FreeCString(ctx, mode); if (ct == MBEDTLS_CIPHER_NONE) return JS_ThrowTypeError(ctx, "modo AES sconosciuto");
  int h = -1; for (int i = 0; i < MAXH; i++) if (!G->ch[i].used) { h = i; break; } if (h < 0) return JS_ThrowInternalError(ctx, "troppi cifrari aperti");
  CHandle *x = &G->ch[h]; mbedtls_cipher_init(&x->c); x->used = 1; x->gcm = gcm; x->enc = enc; x->has_tag = 0;
  if (mbedtls_cipher_setup(&x->c, mbedtls_cipher_info_from_type(ct)) != 0 || mbedtls_cipher_setkey(&x->c, key, (int)kl * 8, enc ? MBEDTLS_ENCRYPT : MBEDTLS_DECRYPT) != 0 || mbedtls_cipher_set_iv(&x->c, iv, il) != 0 || mbedtls_cipher_reset(&x->c) != 0) { mbedtls_cipher_free(&x->c); x->used = 0; return JS_ThrowInternalError(ctx, "inizializzazione AES"); }
  if (!gcm && ct != MBEDTLS_CIPHER_AES_256_CTR && ct != MBEDTLS_CIPHER_AES_128_CTR) mbedtls_cipher_set_padding_mode(&x->c, MBEDTLS_PADDING_PKCS7);   // CBC come in Node: padding PKCS7
  return JS_NewInt32(ctx, h);
}
static JSValue js_cipher_aad(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int h; JS_ToInt32(ctx, &h, argv[0]); if (h < 0 || h >= MAXH || !G->ch[h].used) return JS_ThrowTypeError(ctx, "cifrario non valido");
  NEED_BYTES(p, n, 1); if (mbedtls_cipher_update_ad(&G->ch[h].c, p, n) != 0) return JS_ThrowInternalError(ctx, "AAD"); return JS_UNDEFINED;
}
static JSValue js_cipher_tag(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {   // per la decifratura GCM: imposta il tag atteso
  int h; JS_ToInt32(ctx, &h, argv[0]); if (h < 0 || h >= MAXH || !G->ch[h].used) return JS_ThrowTypeError(ctx, "cifrario non valido");
  NEED_BYTES(p, n, 1); if (n != 16) return JS_ThrowTypeError(ctx, "tag di 16 byte"); memcpy(G->ch[h].tag, p, 16); G->ch[h].has_tag = 1; return JS_UNDEFINED;
}
static JSValue js_cipher_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int h; JS_ToInt32(ctx, &h, argv[0]); if (h < 0 || h >= MAXH || !G->ch[h].used) return JS_ThrowTypeError(ctx, "cifrario non valido");
  NEED_BYTES(p, n, 1); CHandle *x = &G->ch[h]; size_t cap = n + 32, ol = 0; unsigned char *out = malloc(cap);
  if (mbedtls_cipher_update(&x->c, p, n, out, &ol) != 0) { free(out); return JS_ThrowInternalError(ctx, "AES update"); }
  JSValue r = new_ab(ctx, out, ol); free(out); return r;
}
static JSValue js_cipher_final(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int h; JS_ToInt32(ctx, &h, argv[0]); if (h < 0 || h >= MAXH || !G->ch[h].used) return JS_ThrowTypeError(ctx, "cifrario non valido");
  CHandle *x = &G->ch[h]; unsigned char out[64]; size_t ol = 0; int rc = mbedtls_cipher_finish(&x->c, out, &ol);
  JSValue res = JS_UNDEFINED;
  if (rc != 0) { char eb[100]; mbedtls_strerror(rc, eb, sizeof eb); res = JS_ThrowInternalError(ctx, "AES final: %s", eb); }
  else if (x->gcm) {
    if (!x->enc) { if (!x->has_tag || mbedtls_cipher_check_tag(&x->c, x->tag, 16) != 0) res = JS_ThrowInternalError(ctx, "Unsupported state or unable to authenticate data"); else res = new_ab(ctx, out, ol); }
    else { unsigned char tag[16]; if (mbedtls_cipher_write_tag(&x->c, tag, 16) != 0) res = JS_ThrowInternalError(ctx, "tag GCM"); else { JSValue o = JS_NewObject(ctx); JS_SetPropertyStr(ctx, o, "data", new_ab(ctx, out, ol)); JS_SetPropertyStr(ctx, o, "tag", new_ab(ctx, tag, 16)); res = o; } }
  } else res = new_ab(ctx, out, ol);
  mbedtls_cipher_free(&x->c); x->used = 0; return res;
}

// ---------- file ----------
static JSValue js_fs_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *p = JS_ToCString(ctx, argv[0]); if (!p) return JS_EXCEPTION; FILE *f = fopen(p, "rb"); JS_FreeCString(ctx, p); if (!f) return JS_NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); unsigned char *b = malloc(n > 0 ? n : 1); size_t r = fread(b, 1, n > 0 ? n : 0, f); fclose(f);
  JSValue v = new_ab(ctx, b, r); free(b); return v;
}
static JSValue js_fs_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *p = JS_ToCString(ctx, argv[0]); if (!p) return JS_EXCEPTION; NEED_BYTES(d, n, 1);
  char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp", p); FILE *f = fopen(tmp, "wb"); if (!f) { JS_FreeCString(ctx, p); return JS_FALSE; }
  size_t w = n ? fwrite(d, 1, n, f) : 0; fclose(f); int ok = w == n && rename(tmp, p) == 0; JS_FreeCString(ctx, p); return JS_NewBool(ctx, ok);
}
static JSValue js_fs_unlink(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { const char *p = JS_ToCString(ctx, argv[0]); if (!p) return JS_EXCEPTION; int r = remove(p); JS_FreeCString(ctx, p); return JS_NewBool(ctx, r == 0); }
static JSValue js_fs_mkdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *p = JS_ToCString(ctx, argv[0]); if (!p) return JS_EXCEPTION; char b[600]; snprintf(b, sizeof b, "%s", p); JS_FreeCString(ctx, p);
  for (char *q = b + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(b, 0777); *q = '/'; } mkdir(b, 0777); return JS_TRUE;
}
static JSValue js_fs_exists(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { const char *p = JS_ToCString(ctx, argv[0]); if (!p) return JS_EXCEPTION; struct stat st; int r = stat(p, &st) == 0; JS_FreeCString(ctx, p); return JS_NewBool(ctx, r); }
static JSValue js_fs_list(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *p = JS_ToCString(ctx, argv[0]); if (!p) return JS_EXCEPTION; DIR *d = opendir(p); JS_FreeCString(ctx, p); JSValue arr = JS_NewArray(ctx); if (!d) return arr;
  struct dirent *e; uint32_t i = 0; while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, e->d_name)); } closedir(d); return arr;
}

// ---------- zlib ----------
static JSValue js_zlib(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {   // magic 0 = inflate, 1 = deflate
  NEED_BYTES(p, n, 0); z_stream s; memset(&s, 0, sizeof s); int rc = magic ? deflateInit(&s, 6) : inflateInit(&s); if (rc != Z_OK) return JS_ThrowInternalError(ctx, "zlib init");
  size_t cap = n * (magic ? 1 : 4) + 1024, len = 0; unsigned char *out = malloc(cap); s.next_in = p; s.avail_in = (uInt)n;
  for (;;) {
    if (len == cap) { cap *= 2; out = realloc(out, cap); }
    s.next_out = out + len; s.avail_out = (uInt)(cap - len);
    rc = magic ? deflate(&s, Z_FINISH) : inflate(&s, Z_NO_FLUSH); len = cap - s.avail_out;
    if (rc == Z_STREAM_END) break; if (rc != Z_OK && rc != Z_BUF_ERROR) { magic ? deflateEnd(&s) : inflateEnd(&s); free(out); return JS_ThrowInternalError(ctx, "zlib: errore %d", rc); }
    if (rc == Z_BUF_ERROR && s.avail_in == 0 && s.avail_out > 0) { magic ? deflateEnd(&s) : inflateEnd(&s); free(out); return JS_ThrowInternalError(ctx, "zlib: dati troncati"); }
  }
  magic ? deflateEnd(&s) : inflateEnd(&s); JSValue r = new_ab(ctx, out, len); free(out); return r;
}

// ---------- WebSocket sicuro (wss) su mbedTLS ----------
static void ws_free(WS *w) {
  if (!w->used) return; mbedtls_ssl_free(&w->ssl); mbedtls_ssl_config_free(&w->conf); mbedtls_net_free(&w->net);
  free(w->rx); free(w->frag); if (G && G->ctx) JS_FreeValue(G->ctx, w->cb); memset(w, 0, sizeof *w);
}
static int ws_write_all(WS *w, const unsigned char *p, size_t n) {
  size_t off = 0; long long t0 = now_ms();
  while (off < n) {
    int r = mbedtls_ssl_write(&w->ssl, p + off, n - off);
    if (r > 0) { off += r; continue; }
    if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) { if (now_ms() - t0 > 20000) return -1; fd_set fs; FD_ZERO(&fs); FD_SET(w->net.fd, &fs); struct timeval tv = { 0, 50000 }; select(w->net.fd + 1, r == MBEDTLS_ERR_SSL_WANT_READ ? &fs : NULL, r == MBEDTLS_ERR_SSL_WANT_WRITE ? &fs : NULL, NULL, &tv); continue; }
    return -1;
  }
  return 0;
}
static int ws_send_frame(WS *w, int op, const unsigned char *d, size_t n) {
  unsigned char *f = malloc(n + 14); size_t h = 0; f[h++] = 0x80 | op;
  if (n < 126) f[h++] = 0x80 | (unsigned char)n; else if (n < 65536) { f[h++] = 0x80 | 126; f[h++] = n >> 8; f[h++] = n & 255; } else { f[h++] = 0x80 | 127; for (int i = 7; i >= 0; i--) f[h++] = (unsigned char)((unsigned long long)n >> (8 * i)); }
  unsigned char mask[4]; mbedtls_ctr_drbg_random(&G->drbg, mask, 4); memcpy(f + h, mask, 4); h += 4;
  for (size_t i = 0; i < n; i++) f[h + i] = d[i] ^ mask[i & 3];
  int r = ws_write_all(w, f, h + n); free(f); return r;
}
static void ws_event(WS *w, const char *type, const void *data, size_t n, int binary) {
  JSContext *ctx = G->ctx; JSValue args[3] = { JS_NewString(ctx, type), data ? new_ab(ctx, data, n) : JS_UNDEFINED, JS_NewBool(ctx, binary) };
  JSValue r = JS_Call(ctx, w->cb, JS_UNDEFINED, 3, args); if (JS_IsException(r)) { JSValue ex = JS_GetException(ctx); const char *s = JS_ToCString(ctx, ex); elog("errore nel callback ws: %s", s ? s : "?"); JS_FreeCString(ctx, s); JS_FreeValue(ctx, ex); }
  JS_FreeValue(ctx, r); for (int i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
}
// ws_open(url, headers{}, callback) -> id
static JSValue js_ws_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *url = JS_ToCString(ctx, argv[0]); if (!url) return JS_EXCEPTION;
  char host[256], path[1024], port[8] = "443"; const char *u = url; int tls = 1;
  if (!strncmp(u, "wss://", 6)) u += 6; else if (!strncmp(u, "ws://", 5)) { u += 5; tls = 0; strcpy(port, "80"); } else { JS_FreeCString(ctx, url); return JS_ThrowTypeError(ctx, "url non valido"); }
  const char *slash = strchr(u, '/'); size_t hl = slash ? (size_t)(slash - u) : strlen(u); char hp[300]; snprintf(hp, sizeof hp, "%.*s", (int)hl, u);
  snprintf(path, sizeof path, "%s", slash ? slash : "/"); char *colon = strchr(hp, ':'); if (colon) { *colon = 0; snprintf(port, sizeof port, "%s", colon + 1); } snprintf(host, sizeof host, "%s", hp);
  JS_FreeCString(ctx, url);
  if (!tls) return JS_ThrowTypeError(ctx, "solo wss");
  int id = -1; for (int i = 0; i < MAXWS; i++) if (!G->ws[i].used) { id = i; break; } if (id < 0) return JS_ThrowInternalError(ctx, "troppi websocket");
  WS *w = &G->ws[id]; memset(w, 0, sizeof *w); w->used = 1; w->cb = JS_DupValue(ctx, argv[2]);
  mbedtls_ssl_init(&w->ssl); mbedtls_ssl_config_init(&w->conf); mbedtls_net_init(&w->net);
  int rc; char eb[160];
  if ((rc = mbedtls_net_connect(&w->net, host, port, MBEDTLS_NET_PROTO_TCP)) != 0) { ws_free(w); return JS_ThrowInternalError(ctx, "connessione a %s fallita (%d)", host, rc); }
  mbedtls_ssl_config_defaults(&w->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  mbedtls_ssl_conf_rng(&w->conf, mbedtls_ctr_drbg_random, &G->drbg);
  mbedtls_ssl_conf_max_tls_version(&w->conf, MBEDTLS_SSL_VERSION_TLS1_2);   // WhatsApp supporta TLS 1.2: piu' leggero e uguale su PC e Vita
  if (G->ca_ok) { mbedtls_ssl_conf_ca_chain(&w->conf, &G->ca, NULL); mbedtls_ssl_conf_authmode(&w->conf, MBEDTLS_SSL_VERIFY_REQUIRED); } else mbedtls_ssl_conf_authmode(&w->conf, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_read_timeout(&w->conf, 15000);
  if ((rc = mbedtls_ssl_setup(&w->ssl, &w->conf)) != 0 || (rc = mbedtls_ssl_set_hostname(&w->ssl, host)) != 0) { ws_free(w); return JS_ThrowInternalError(ctx, "ssl_setup"); }
  mbedtls_ssl_set_bio(&w->ssl, &w->net, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);
  while ((rc = mbedtls_ssl_handshake(&w->ssl)) != 0) { if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) { mbedtls_strerror(rc, eb, sizeof eb); uint32_t fl = mbedtls_ssl_get_verify_result(&w->ssl); ws_free(w); return JS_ThrowInternalError(ctx, "TLS: %s (verifica 0x%x)", eb, (unsigned)fl); } }
  // richiesta di upgrade
  unsigned char kraw[16]; char key[32]; size_t kl; mbedtls_ctr_drbg_random(&G->drbg, kraw, 16); mbedtls_base64_encode((unsigned char *)key, sizeof key, &kl, kraw, 16); key[kl] = 0;
  char req[4096]; int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n", path, host, key);
  if (argc > 1 && JS_IsObject(argv[1])) {
    JSPropertyEnum *tab; uint32_t cnt; if (JS_GetOwnPropertyNames(ctx, &tab, &cnt, argv[1], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
      for (uint32_t i = 0; i < cnt; i++) { const char *k = JS_AtomToCString(ctx, tab[i].atom); JSValue v = JS_GetProperty(ctx, argv[1], tab[i].atom); const char *vs = JS_ToCString(ctx, v); if (k && vs && n < (int)sizeof req - 600) n += snprintf(req + n, sizeof req - n, "%s: %s\r\n", k, vs); JS_FreeCString(ctx, vs); JS_FreeCString(ctx, k); JS_FreeValue(ctx, v); JS_FreeAtom(ctx, tab[i].atom); }
      js_free(ctx, tab);
    }
  }
  n += snprintf(req + n, sizeof req - n, "\r\n");
  if (ws_write_all(w, (unsigned char *)req, n) != 0) { ws_free(w); return JS_ThrowInternalError(ctx, "invio richiesta"); }
  char resp[4096]; size_t rl = 0; long long t0 = now_ms();
  while (rl < sizeof resp - 1 && now_ms() - t0 < 20000) { int r = mbedtls_ssl_read(&w->ssl, (unsigned char *)resp + rl, 1); if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue; if (r <= 0) break; rl++; if (rl >= 4 && !memcmp(resp + rl - 4, "\r\n\r\n", 4)) break; }
  resp[rl] = 0; if (strncmp(resp, "HTTP/1.1 101", 12) != 0) { char first[120]; snprintf(first, sizeof first, "%.100s", resp); char *nl = strchr(first, '\r'); if (nl) *nl = 0; ws_free(w); return JS_ThrowInternalError(ctx, "upgrade WebSocket rifiutato: %s", first); }
  mbedtls_net_set_nonblock(&w->net); mbedtls_ssl_set_bio(&w->ssl, &w->net, mbedtls_net_send, mbedtls_net_recv, NULL); w->state = 1; w->fd = w->net.fd;   // da qui letture non bloccanti
  return JS_NewInt32(ctx, id);
}
static JSValue js_ws_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int id; JS_ToInt32(ctx, &id, argv[0]); if (id < 0 || id >= MAXWS || G->ws[id].state != 1) return JS_FALSE;
  NEED_BYTES(p, n, 1); int bin = argc > 2 ? JS_ToBool(ctx, argv[2]) : 1; return JS_NewBool(ctx, ws_send_frame(&G->ws[id], bin ? 2 : 1, p, n) == 0);
}
static JSValue js_ws_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int id; JS_ToInt32(ctx, &id, argv[0]); if (id < 0 || id >= MAXWS || !G->ws[id].used) return JS_UNDEFINED; WS *w = &G->ws[id];
  if (w->state == 1) { unsigned char c[2] = { 0x03, 0xe8 }; ws_send_frame(w, 8, c, 2); mbedtls_ssl_close_notify(&w->ssl); }
  w->state = 2; return JS_UNDEFINED;   // il callback 'close' parte dal ciclo eventi
}
static void ws_service(WS *w) {
  if (w->state == 2) { if (!w->closed_notified) { w->closed_notified = 1; ws_event(w, "close", NULL, 0, 0); } ws_free(w); return; }
  for (int guard = 0; guard < 64; guard++) {
    if (w->rxlen + 8192 > w->rxcap) { w->rxcap = w->rxcap ? w->rxcap * 2 : 65536; w->rx = realloc(w->rx, w->rxcap); }
    int r = mbedtls_ssl_read(&w->ssl, w->rx + w->rxlen, w->rxcap - w->rxlen);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) break;
    if (r <= 0) { if (!w->closed_notified) { w->closed_notified = 1; w->state = 2; ws_event(w, "close", NULL, 0, 0); } ws_free(w); return; }
    w->rxlen += r;
  }
  size_t off = 0;
  while (w->rxlen - off >= 2) {
    unsigned char *f = w->rx + off; int op = f[0] & 15, fin = f[0] >> 7, masked = f[1] >> 7; unsigned long long len = f[1] & 127; size_t h = 2;
    if (len == 126) { if (w->rxlen - off < 4) break; len = (f[2] << 8) | f[3]; h = 4; } else if (len == 127) { if (w->rxlen - off < 10) break; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | f[2 + i]; h = 10; }
    unsigned char mk[4] = { 0 }; if (masked) { if (w->rxlen - off < h + 4) break; memcpy(mk, f + h, 4); h += 4; }
    if (len > (64u << 20)) { w->state = 2; break; }
    if (w->rxlen - off < h + len) break;
    unsigned char *pl = f + h; if (masked) for (unsigned long long i = 0; i < len; i++) pl[i] ^= mk[i & 3];
    if (op == 9) ws_send_frame(w, 10, pl, (size_t)len);
    else if (op == 8) { w->state = 2; off += h + (size_t)len; break; }
    else if (op == 1 || op == 2 || op == 0) {
      if (op != 0) { w->fraglen = 0; w->fragop = op; }
      w->frag = realloc(w->frag, w->fraglen + (size_t)len + 1); memcpy(w->frag + w->fraglen, pl, (size_t)len); w->fraglen += (size_t)len;
      if (fin) { size_t fl = w->fraglen; unsigned char *copy = malloc(fl + 1); memcpy(copy, w->frag, fl); w->fraglen = 0; ws_event(w, "message", copy, fl, w->fragop == 2); free(copy); if (!w->used) return; }
    }
    off += h + (size_t)len;
  }
  if (off) { memmove(w->rx, w->rx + off, w->rxlen - off); w->rxlen -= off; }
}

// ---------- ciclo eventi ----------
static void report_exception(JSContext *ctx, const char *where) {
  JSValue ex = JS_GetException(ctx); const char *s = JS_ToCString(ctx, ex); JSValue st = JS_GetPropertyStr(ctx, ex, "stack"); const char *ss = JS_IsUndefined(st) ? NULL : JS_ToCString(ctx, st);
  elog("ERRORE JS (%s): %s%s%s", where, s ? s : "?", ss ? "\n" : "", ss ? ss : ""); JS_FreeCString(ctx, s); JS_FreeCString(ctx, ss); JS_FreeValue(ctx, st); JS_FreeValue(ctx, ex);
}
static void drain_jobs(Engine *e) { JSContext *c1; int r; while ((r = JS_ExecutePendingJob(e->rt, &c1)) != 0) { if (r < 0) report_exception(c1, "job"); } }
int eng_loop_step(Engine *e, int max_wait_ms) {
  drain_jobs(e);
  long long now = now_ms(); Timer *best = NULL; for (Timer *t = e->timers; t; t = t->next) if (!best || t->due < best->due) best = t;
  long long wait = max_wait_ms; if (best) { long long d = best->due - now; if (d < 0) d = 0; if (d < wait) wait = d; }
  int nfd = 0, maxfd = -1; fd_set rs; FD_ZERO(&rs); int pending_ssl = 0;
  for (int i = 0; i < MAXWS; i++) { WS *w = &e->ws[i]; if (!w->used) continue; nfd++; if (w->state == 2) pending_ssl = 1; else { FD_SET(w->net.fd, &rs); if (w->net.fd > maxfd) maxfd = w->net.fd; if (mbedtls_ssl_get_bytes_avail(&w->ssl) > 0) pending_ssl = 1; } }
  if (pending_ssl) wait = 0;
  if (maxfd >= 0 || wait > 0) { struct timeval tv = { (long)(wait / 1000), (long)((wait % 1000) * 1000) }; select(maxfd + 1, maxfd >= 0 ? &rs : NULL, NULL, NULL, &tv); }
  for (int i = 0; i < MAXWS; i++) { WS *w = &e->ws[i]; if (w->used) { ws_service(w); drain_jobs(e); } }
  now = now_ms();
  for (;;) {
    Timer *due = NULL; for (Timer *t = e->timers; t; t = t->next) if (t->due <= now && (!due || t->due < due->due)) due = t; if (!due) break;
    JSValue fn = JS_DupValue(e->ctx, due->fn);
    if (due->every) due->due = now + due->every; else { for (Timer **pp = &e->timers; *pp; pp = &(*pp)->next) if (*pp == due) { *pp = due->next; break; } JS_FreeValue(e->ctx, due->fn); free(due); }
    JSValue r = JS_Call(e->ctx, fn, JS_UNDEFINED, 0, NULL); if (JS_IsException(r)) report_exception(e->ctx, "timer"); JS_FreeValue(e->ctx, r); JS_FreeValue(e->ctx, fn); drain_jobs(e);
    now = now_ms();
  }
  int work = e->timers != NULL; for (int i = 0; i < MAXWS; i++) if (e->ws[i].used) work = 1;
  return work;
}

// ---------- moduli ES ----------
static char *read_file(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET); char *b = malloc(l + 1); size_t r = fread(b, 1, l, f); fclose(f); b[r] = 0; *n = r; return b; }
static char *mod_normalize(JSContext *ctx, const char *base, const char *name, void *opaque) {
  if (name[0] != '.') return js_strdup(ctx, name);
  char dir[600]; snprintf(dir, sizeof dir, "%s", base); char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else strcpy(dir, ".");
  char out[1200]; snprintf(out, sizeof out, "%s/%s", dir, name[1] == '/' ? name + 2 : name); return js_strdup(ctx, out);
}
static JSModuleDef *mod_load(JSContext *ctx, const char *name, void *opaque) {
  size_t n; char *src = read_file(name, &n); if (!src) { JS_ThrowReferenceError(ctx, "modulo non trovato: %s", name); return NULL; }
  JSValue v = JS_Eval(ctx, src, n, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY); free(src); if (JS_IsException(v)) return NULL;
  JSModuleDef *m = JS_VALUE_GET_PTR(v); JS_FreeValue(ctx, v); return m;
}
static void rejection_tracker(JSContext *ctx, JSValueConst promise, JSValueConst reason, bool is_handled, void *opaque) {
  if (is_handled) return; const char *s = JS_ToCString(ctx, reason); JSValue st = JS_IsObject(reason) ? JS_GetPropertyStr(ctx, reason, "stack") : JS_UNDEFINED; const char *ss = JS_IsUndefined(st) ? NULL : JS_ToCString(ctx, st);
  elog("PROMISE RIFIUTATA senza gestione: %s%s%s", s ? s : "?", ss ? "\n" : "", ss ? ss : ""); JS_FreeCString(ctx, s); JS_FreeCString(ctx, ss); JS_FreeValue(ctx, st);
}
int eng_run_module(Engine *e, const char *path) {
  size_t n; char *src = read_file(path, &n); if (!src) { elog("impossibile leggere %s", path); return -1; }
  JSValue v = JS_Eval(e->ctx, src, n, path, JS_EVAL_TYPE_MODULE); free(src);
  if (JS_IsException(v)) { report_exception(e->ctx, path); return -1; }
  drain_jobs(e); JS_FreeValue(e->ctx, v); return 0;
}

#define FN(name, fn, n) JS_SetPropertyStr(ctx, nat, name, JS_NewCFunction(ctx, fn, name, n))
Engine *eng_new(const char *cacert_path, const char *data_dir, EngineLog log) {
  Engine *e = calloc(1, sizeof *e); G = e; e->log = log; snprintf(e->cacert, sizeof e->cacert, "%s", cacert_path ? cacert_path : ""); snprintf(e->datadir, sizeof e->datadir, "%s", data_dir ? data_dir : ".");
  psa_crypto_init();   // serve a TLS 1.3 in mbedTLS 3.6
  mbedtls_entropy_init(&e->entropy); mbedtls_ctr_drbg_init(&e->drbg); mbedtls_x509_crt_init(&e->ca);
  mbedtls_ctr_drbg_seed(&e->drbg, mbedtls_entropy_func, &e->entropy, (const unsigned char *)"vita-wa-engine", 14);
  if (e->cacert[0] && mbedtls_x509_crt_parse_file(&e->ca, e->cacert) >= 0) e->ca_ok = 1; else elog("attenzione: certificati CA non caricati (%s)", e->cacert);
  e->rt = JS_NewRuntime(); JS_SetMemoryLimit(e->rt, 200u << 20); JS_SetMaxStackSize(e->rt, 1u << 20); e->ctx = JS_NewContext(e->rt); JSContext *ctx = e->ctx;
  JS_SetModuleLoaderFunc(e->rt, mod_normalize, mod_load, NULL); JS_SetHostPromiseRejectionTracker(e->rt, rejection_tracker, NULL);
  JSValue g = JS_GetGlobalObject(ctx), nat = JS_NewObject(ctx);
  JSValue con = JS_NewObject(ctx); JS_SetPropertyStr(ctx, con, "log", JS_NewCFunction(ctx, js_log, "log", 1)); JS_SetPropertyStr(ctx, g, "console", con);
  JS_SetPropertyStr(ctx, con, "info", JS_NewCFunction(ctx, js_log, "info", 1)); JS_SetPropertyStr(ctx, con, "warn", JS_NewCFunction(ctx, js_log, "warn", 1)); JS_SetPropertyStr(ctx, con, "error", JS_NewCFunction(ctx, js_log, "error", 1)); JS_SetPropertyStr(ctx, con, "debug", JS_NewCFunction(ctx, js_log, "debug", 1));
  JS_SetPropertyStr(ctx, g, "setTimeout", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_set_timer, "setTimeout", 2, JS_CFUNC_generic_magic, 0));
  JS_SetPropertyStr(ctx, g, "setInterval", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_set_timer, "setInterval", 2, JS_CFUNC_generic_magic, 1));
  JS_SetPropertyStr(ctx, g, "clearTimeout", JS_NewCFunction(ctx, js_clear_timer, "clearTimeout", 1)); JS_SetPropertyStr(ctx, g, "clearInterval", JS_NewCFunction(ctx, js_clear_timer, "clearInterval", 1));
  FN("now", js_now, 0); FN("mono", js_mono, 0); FN("randomBytes", js_random, 1);
  FN("hashNew", js_hash_new, 2); FN("hashUpdate", js_hash_update, 2); FN("hashFinal", js_hash_final, 1);
  FN("cipherNew", js_cipher_new, 4); FN("cipherAad", js_cipher_aad, 2); FN("cipherTag", js_cipher_tag, 2); FN("cipherUpdate", js_cipher_update, 2); FN("cipherFinal", js_cipher_final, 1);
  FN("fsRead", js_fs_read, 1); FN("fsWrite", js_fs_write, 2); FN("fsUnlink", js_fs_unlink, 1); FN("fsMkdir", js_fs_mkdir, 1); FN("fsExists", js_fs_exists, 1); FN("fsList", js_fs_list, 1);
  JS_SetPropertyStr(ctx, nat, "inflate", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_zlib, "inflate", 1, JS_CFUNC_generic_magic, 0)); JS_SetPropertyStr(ctx, nat, "deflate", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_zlib, "deflate", 1, JS_CFUNC_generic_magic, 1));
  FN("wsOpen", js_ws_open, 3); FN("wsSend", js_ws_send, 3); FN("wsClose", js_ws_close, 1);
  JS_SetPropertyStr(ctx, nat, "dataDir", JS_NewString(ctx, e->datadir));
  JS_SetPropertyStr(ctx, g, "__nat", nat); JS_FreeValue(ctx, g);
  return e;
}
void eng_free(Engine *e) { if (!e) return; for (int i = 0; i < MAXWS; i++) ws_free(&e->ws[i]); JS_FreeContext(e->ctx); JS_FreeRuntime(e->rt); free(e); }
