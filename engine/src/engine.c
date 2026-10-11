#include "engine.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <zlib.h>
#include <pthread.h>
#include <malloc.h>
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
#include "curve25519.h"
#include "image.h"
#include "audio.h"

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


static JSValue js_fs_stat(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *p = JS_ToCString(ctx, argv[0]); if (!p) return JS_EXCEPTION; struct stat st; int r = stat(p, &st); JS_FreeCString(ctx, p); if (r != 0) return JS_NULL;
  JSValue o = JS_NewObject(ctx); JS_SetPropertyStr(ctx, o, "size", JS_NewFloat64(ctx, (double)st.st_size)); JS_SetPropertyStr(ctx, o, "mtimeMs", JS_NewFloat64(ctx, (double)st.st_mtime * 1000.0)); JS_SetPropertyStr(ctx, o, "isDir", JS_NewBool(ctx, S_ISDIR(st.st_mode))); return o;
}
static JSValue js_fs_rename(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { const char *a = JS_ToCString(ctx, argv[0]), *b = JS_ToCString(ctx, argv[1]); int r = (a && b) ? rename(a, b) : -1; JS_FreeCString(ctx, a); JS_FreeCString(ctx, b); return JS_NewBool(ctx, r == 0); }
static JSValue js_fs_append(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *p = JS_ToCString(ctx, argv[0]); if (!p) return JS_EXCEPTION; NEED_BYTES(d, n, 1); FILE *f = fopen(p, "ab"); JS_FreeCString(ctx, p); if (!f) return JS_FALSE; size_t w = n ? fwrite(d, 1, n, f) : 0; fclose(f); return JS_NewBool(ctx, w == n);
}
static JSValue js_fs_read_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {   // fsReadRange(path, offset, len) -> ArrayBuffer (anche vuoto a fine file) o null
  const char *p = JS_ToCString(ctx, argv[0]); if (!p) return JS_EXCEPTION; double off = 0, len = 0; JS_ToFloat64(ctx, &off, argv[1]); JS_ToFloat64(ctx, &len, argv[2]);
  FILE *f = fopen(p, "rb"); JS_FreeCString(ctx, p); if (!f) return JS_NULL; fseek(f, (long)off, SEEK_SET); size_t n = (size_t)len; unsigned char *b = malloc(n ? n : 1); size_t r = fread(b, 1, n, f); fclose(f); JSValue v = new_ab(ctx, b, r); free(b); return v;
}


// ---------- curve25519 / firme (nativo: la versione JavaScript era il collo di bottiglia) ----------
static JSValue js_curve_keypair(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { size_t n = 0; uint8_t *seed = get_bytes(ctx, argv[0], &n); if (!seed || n != 32) return JS_ThrowTypeError(ctx, "seed di 32 byte"); uint8_t pub[32], priv[32]; c25519_keypair(seed, pub, priv); JSValue o = JS_NewObject(ctx); JS_SetPropertyStr(ctx, o, "public", new_ab(ctx, pub, 32)); JS_SetPropertyStr(ctx, o, "private", new_ab(ctx, priv, 32)); return o; }
static JSValue js_curve_shared(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { size_t a = 0, b = 0; uint8_t *sk = get_bytes(ctx, argv[0], &a), *pk = get_bytes(ctx, argv[1], &b); if (!sk || !pk || a != 32 || b != 32) return JS_ThrowTypeError(ctx, "chiavi di 32 byte"); uint8_t out[32]; c25519_shared(out, sk, pk); return new_ab(ctx, out, 32); }
static JSValue js_curve_sign(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { size_t a = 0, m = 0, r = 0; uint8_t *sk = get_bytes(ctx, argv[0], &a), *msg = get_bytes(ctx, argv[1], &m); uint8_t *rnd = (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) ? get_bytes(ctx, argv[2], &r) : NULL; if (!sk || a != 32 || (!msg && m)) return JS_ThrowTypeError(ctx, "argomenti non validi"); if (rnd && r != 64) return JS_ThrowTypeError(ctx, "servono 64 byte casuali"); uint8_t sig[64]; c25519_sign(sig, sk, msg ? msg : (uint8_t *)"", m, rnd); return new_ab(ctx, sig, 64); }
static JSValue js_curve_verify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { size_t a = 0, m = 0, s = 0; uint8_t *pk = get_bytes(ctx, argv[0], &a), *msg = get_bytes(ctx, argv[1], &m), *sig = get_bytes(ctx, argv[2], &s); if (!pk || a != 32 || !sig || s != 64) return JS_ThrowTypeError(ctx, "argomenti non validi"); return JS_NewBool(ctx, c25519_verify(pk, msg ? msg : (uint8_t *)"", m, sig)); }


// ---------- immagini (sostituisce 'sharp') ----------
// imgConvert(buf, maxW, maxH, fit(0|1), fmt(0 jpeg|1 png), quality, rotate) -> { data, width, height } | null
static JSValue js_img_convert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  size_t n = 0; uint8_t *in = get_bytes(ctx, argv[0], &n); if (!in) return JS_ThrowTypeError(ctx, "serve un buffer");
  int a[6] = { 0, 0, 0, 0, 80, 1 }; for (int i = 0; i < 6; i++) if (argc > i + 1 && !JS_IsUndefined(argv[i + 1]) && !JS_IsNull(argv[i + 1])) JS_ToInt32(ctx, &a[i], argv[i + 1]);
  uint8_t *out = NULL; size_t on = 0; int w = 0, h = 0; if (img_convert(in, n, a[0], a[1], a[2], a[3], a[4], a[5], &out, &on, &w, &h) != 0) return JS_NULL;
  JSValue o = JS_NewObject(ctx); JS_SetPropertyStr(ctx, o, "data", new_ab(ctx, out, on)); JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, w)); JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, h)); free(out); return o;
}
static JSValue js_img_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  size_t n = 0; uint8_t *in = get_bytes(ctx, argv[0], &n); if (!in) return JS_NULL; ImgInfo i; if (img_info(in, n, &i) != 0) return JS_NULL;
  JSValue o = JS_NewObject(ctx); JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, i.w)); JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, i.h)); JS_SetPropertyStr(ctx, o, "format", JS_NewString(ctx, i.fmt)); return o;
}


// ---------- audio / video (al posto di FFmpeg) ----------
static JSValue js_audio_to_wav(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {   // -> { data, seconds } | null
  size_t n = 0; uint8_t *in = get_bytes(ctx, argv[0], &n); if (!in) return JS_NULL; uint8_t *out = NULL; size_t on = 0; double sec = 0;
  if (audio_ogg_opus_to_wav(in, n, &out, &on, &sec) != 0) return JS_NULL; JSValue o = JS_NewObject(ctx); JS_SetPropertyStr(ctx, o, "data", new_ab(ctx, out, on)); JS_SetPropertyStr(ctx, o, "seconds", JS_NewFloat64(ctx, sec)); free(out); return o;
}
static JSValue js_video_playable(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  size_t n = 0; uint8_t *in = get_bytes(ctx, argv[0], &n); if (!in) return JS_FALSE; return JS_NewBool(ctx, video_mp4_playable(in, n));
}
static JSValue js_fs_copy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {   // copia a blocchi (video grandi)
  const char *a = JS_ToCString(ctx, argv[0]), *b = JS_ToCString(ctx, argv[1]); int ok = 0; if (a && b) { FILE *f = fopen(a, "rb"), *g = fopen(b, "wb"); if (f && g) { unsigned char *buf = malloc(65536); size_t k; ok = 1; while ((k = fread(buf, 1, 65536, f)) > 0) if (fwrite(buf, 1, k, g) != k) { ok = 0; break; } free(buf); } if (f) fclose(f); if (g) fclose(g); }
  JS_FreeCString(ctx, a); JS_FreeCString(ctx, b); return JS_NewBool(ctx, ok);
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
  JSValue cb = JS_DupValue(ctx, w->cb);
  JSValue r = JS_Call(ctx, cb, JS_UNDEFINED, 3, args); JS_FreeValue(ctx, cb); if (JS_IsException(r)) { JSValue ex = JS_GetException(ctx); const char *s = JS_ToCString(ctx, ex); elog("errore nel callback ws: %s", s ? s : "?"); JS_FreeCString(ctx, s); JS_FreeValue(ctx, ex); }
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


// ---------- client HTTP/HTTPS (fetch) con ricezione a pezzi ----------
#define MAXHTTP 12
typedef struct {
  int used, tls, state;                 // state: 1 attendo intestazioni, 2 corpo, 3 fine
  mbedtls_ssl_context ssl; mbedtls_ssl_config conf; mbedtls_net_context net;
  unsigned char *rx; size_t rxlen, rxcap;
  long long clen; int chunked; long long chunk_left; int in_chunk_hdr; int eof_body;
  JSValue cb; long long t_last;
} Http;
static Http G_http[MAXHTTP];
static void http_free(Http *h) {
  if (!h->used) return; if (h->tls) { mbedtls_ssl_free(&h->ssl); mbedtls_ssl_config_free(&h->conf); } mbedtls_net_free(&h->net);
  free(h->rx); if (G && G->ctx) JS_FreeValue(G->ctx, h->cb); memset(h, 0, sizeof *h);
}
static int http_rd(Http *h, unsigned char *b, size_t n) { return h->tls ? mbedtls_ssl_read(&h->ssl, b, n) : mbedtls_net_recv(&h->net, b, n); }
static int http_wr_all(Http *h, const unsigned char *p, size_t n) {
  size_t off = 0; long long t0 = now_ms();
  while (off < n) {
    int r = h->tls ? mbedtls_ssl_write(&h->ssl, p + off, n - off) : mbedtls_net_send(&h->net, p + off, n - off);
    if (r > 0) { off += r; continue; }
    if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_NET_SEND_FAILED + 1000) { if (now_ms() - t0 > 60000) return -1; fd_set fs; FD_ZERO(&fs); FD_SET(h->net.fd, &fs); struct timeval tv = { 0, 50000 }; select(h->net.fd + 1, NULL, &fs, NULL, &tv); continue; }
    return -1;
  }
  return 0;
}
static void http_ev(Http *h, const char *type, JSValue a, JSValue b) {
  JSContext *ctx = G->ctx; JSValue args[3] = { JS_NewString(ctx, type), a, b };
  JSValue cb = JS_DupValue(ctx, h->cb);   /* il callback puo' liberare la richiesta: teniamo noi un riferimento */
  JSValue r = JS_Call(ctx, cb, JS_UNDEFINED, 3, args); JS_FreeValue(ctx, cb); if (JS_IsException(r)) { JSValue ex = JS_GetException(ctx); const char *s = JS_ToCString(ctx, ex); elog("errore nel callback http: %s", s ? s : "?"); JS_FreeCString(ctx, s); JS_FreeValue(ctx, ex); }
  JS_FreeValue(ctx, r); for (int i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
}
// http_request(method, url, headers{}, body|null, callback(type, a, b)) -> id
static JSValue js_http_request(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *method = JS_ToCString(ctx, argv[0]), *url = JS_ToCString(ctx, argv[1]); if (!method || !url) return JS_EXCEPTION;
  char host[256], path[2048], port[8]; const char *u = url; int tls = 1;
  if (!strncmp(u, "https://", 8)) { u += 8; strcpy(port, "443"); } else if (!strncmp(u, "http://", 7)) { u += 7; tls = 0; strcpy(port, "80"); } else { JS_FreeCString(ctx, method); JS_FreeCString(ctx, url); return JS_ThrowTypeError(ctx, "url non valido"); }
  const char *slash = strchr(u, '/'); size_t hl = slash ? (size_t)(slash - u) : strlen(u); char hp[300]; snprintf(hp, sizeof hp, "%.*s", (int)hl, u);
  snprintf(path, sizeof path, "%s", slash ? slash : "/"); char *colon = strchr(hp, ':'); if (colon) { *colon = 0; snprintf(port, sizeof port, "%s", colon + 1); } snprintf(host, sizeof host, "%s", hp);
  int id = -1; for (int i = 0; i < MAXHTTP; i++) if (!G_http[i].used) { id = i; break; }
  if (id < 0) { JS_FreeCString(ctx, method); JS_FreeCString(ctx, url); return JS_ThrowInternalError(ctx, "troppe richieste HTTP contemporanee"); }
  Http *h = &G_http[id]; memset(h, 0, sizeof *h); h->used = 1; h->tls = tls; h->cb = JS_DupValue(ctx, argv[4]); h->t_last = now_ms();
  mbedtls_ssl_init(&h->ssl); mbedtls_ssl_config_init(&h->conf); mbedtls_net_init(&h->net); int rc; char eb[160];
  if ((rc = mbedtls_net_connect(&h->net, host, port, MBEDTLS_NET_PROTO_TCP)) != 0) { JS_FreeCString(ctx, method); JS_FreeCString(ctx, url); http_free(h); return JS_ThrowInternalError(ctx, "connessione a %s fallita (%d)", host, rc); }
  if (tls) {
    mbedtls_ssl_config_defaults(&h->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT); mbedtls_ssl_conf_rng(&h->conf, mbedtls_ctr_drbg_random, &G->drbg);
    mbedtls_ssl_conf_max_tls_version(&h->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    if (G->ca_ok) { mbedtls_ssl_conf_ca_chain(&h->conf, &G->ca, NULL); mbedtls_ssl_conf_authmode(&h->conf, MBEDTLS_SSL_VERIFY_REQUIRED); } else mbedtls_ssl_conf_authmode(&h->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_read_timeout(&h->conf, 15000);
    mbedtls_ssl_setup(&h->ssl, &h->conf); mbedtls_ssl_set_hostname(&h->ssl, host); mbedtls_ssl_set_bio(&h->ssl, &h->net, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);
    while ((rc = mbedtls_ssl_handshake(&h->ssl)) != 0) if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) { mbedtls_strerror(rc, eb, sizeof eb); JS_FreeCString(ctx, method); JS_FreeCString(ctx, url); http_free(h); return JS_ThrowInternalError(ctx, "TLS: %s", eb); }
    mbedtls_ssl_set_bio(&h->ssl, &h->net, mbedtls_net_send, mbedtls_net_recv, NULL);
  }
  size_t bl = 0; uint8_t *body = (argc > 3 && !JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3])) ? get_bytes(ctx, argv[3], &bl) : NULL;
  size_t cap = 8192; char *req = malloc(cap); size_t n = snprintf(req, cap, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n", method, path, host);
  JS_FreeCString(ctx, method); JS_FreeCString(ctx, url); int has_len = 0;
  if (argc > 2 && JS_IsObject(argv[2])) {
    JSPropertyEnum *tab; uint32_t cnt; if (JS_GetOwnPropertyNames(ctx, &tab, &cnt, argv[2], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
      for (uint32_t i = 0; i < cnt; i++) { const char *k = JS_AtomToCString(ctx, tab[i].atom); JSValue v = JS_GetProperty(ctx, argv[2], tab[i].atom); const char *vs = JS_ToCString(ctx, v);
        if (k && vs && strcasecmp(k, "host") && strcasecmp(k, "connection") && strcasecmp(k, "accept-encoding")) { if (!strcasecmp(k, "content-length")) has_len = 1; if (n + strlen(k) + strlen(vs) + 8 > cap) { cap *= 2; req = realloc(req, cap); } n += snprintf(req + n, cap - n, "%s: %s\r\n", k, vs); }
        JS_FreeCString(ctx, vs); JS_FreeCString(ctx, k); JS_FreeValue(ctx, v); JS_FreeAtom(ctx, tab[i].atom); }
      js_free(ctx, tab);
    }
  }
  if (body && !has_len) n += snprintf(req + n, cap - n, "Content-Length: %zu\r\n", bl);
  n += snprintf(req + n, cap - n, "\r\n");
  int wr = http_wr_all(h, (unsigned char *)req, n); free(req);
  if (wr == 0 && body && bl) wr = http_wr_all(h, body, bl);
  if (wr != 0) { http_free(h); return JS_ThrowInternalError(ctx, "invio richiesta HTTP fallito"); }
  mbedtls_net_set_nonblock(&h->net); h->state = 1; h->clen = -1; return JS_NewInt32(ctx, id);
}
static JSValue js_http_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { int id; JS_ToInt32(ctx, &id, argv[0]); if (id >= 0 && id < MAXHTTP) http_free(&G_http[id]); return JS_UNDEFINED; }
static void http_fail(Http *h, const char *msg) { JSContext *ctx = G->ctx; http_ev(h, "error", JS_NewString(ctx, msg), JS_UNDEFINED); http_free(h); }
static void http_deliver(Http *h, const unsigned char *p, size_t n) { if (n) http_ev(h, "data", new_ab(G->ctx, p, n), JS_UNDEFINED); }
static void http_service(Http *h) {
  JSContext *ctx = G->ctx;
  for (int guard = 0; guard < 64 && h->used; guard++) {
    if (h->rxlen + 16384 > h->rxcap) { h->rxcap = h->rxcap ? h->rxcap * 2 : 65536; h->rx = realloc(h->rx, h->rxcap); }
    int r = http_rd(h, h->rx + h->rxlen, h->rxcap - h->rxlen);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) { if (now_ms() - h->t_last > 60000) { http_fail(h, "timeout"); return; } break; }
    if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r < 0) { h->eof_body = 1; break; }
    h->rxlen += r; h->t_last = now_ms();
  }
  if (!h->used) return;
  for (;;) {
    if (h->state == 1) {
      unsigned char *e = NULL; for (size_t i = 3; i < h->rxlen; i++) if (h->rx[i] == '\n' && h->rx[i - 1] == '\r' && h->rx[i - 2] == '\n' && h->rx[i - 3] == '\r') { e = h->rx + i + 1; break; }
      if (!e) { if (h->eof_body) http_fail(h, "connessione chiusa prima delle intestazioni"); return; }
      size_t hl = e - h->rx; char *hdr = malloc(hl + 1); memcpy(hdr, h->rx, hl); hdr[hl] = 0;
      int status = 0; sscanf(hdr, "HTTP/%*s %d", &status);
      JSValue ho = JS_NewObject(ctx); h->clen = -1; h->chunked = 0;
      char *line = strstr(hdr, "\r\n"); line = line ? line + 2 : hdr + hl;
      while (*line && !(line[0] == '\r' && line[1] == '\n')) {
        char *eol = strstr(line, "\r\n"); if (!eol) break; *eol = 0; char *c = strchr(line, ':');
        if (c) { *c = 0; char *v = c + 1; while (*v == ' ') v++; for (char *k = line; *k; k++) if (*k >= 'A' && *k <= 'Z') *k += 32;
          JS_SetPropertyStr(ctx, ho, line, JS_NewString(ctx, v)); if (!strcmp(line, "content-length")) h->clen = atoll(v); if (!strcmp(line, "transfer-encoding") && strstr(v, "chunked")) h->chunked = 1; }
        line = eol + 2;
      }
      free(hdr); memmove(h->rx, h->rx + hl, h->rxlen - hl); h->rxlen -= hl; h->state = 2; h->in_chunk_hdr = 1; h->chunk_left = 0;
      http_ev(h, "headers", JS_NewInt32(ctx, status), ho); if (!h->used) return; continue;
    }
    if (h->state == 2) {
      if (h->chunked) {
        for (;;) {
          if (h->in_chunk_hdr) {
            unsigned char *nl = memchr(h->rx, '\n', h->rxlen); if (!nl) break; long long sz = strtoll((char *)h->rx, NULL, 16); size_t used = (nl - h->rx) + 1; memmove(h->rx, h->rx + used, h->rxlen - used); h->rxlen -= used;
            if (sz == 0) { http_ev(h, "end", JS_UNDEFINED, JS_UNDEFINED); http_free(h); return; } h->chunk_left = sz; h->in_chunk_hdr = 0;
          }
          if (h->chunk_left > 0) { size_t take = h->rxlen < (size_t)h->chunk_left ? h->rxlen : (size_t)h->chunk_left; if (!take) break; http_deliver(h, h->rx, take); if (!h->used) return; memmove(h->rx, h->rx + take, h->rxlen - take); h->rxlen -= take; h->chunk_left -= take; if (h->chunk_left > 0) break; }
          if (h->chunk_left == 0 && !h->in_chunk_hdr) { if (h->rxlen < 2) break; memmove(h->rx, h->rx + 2, h->rxlen - 2); h->rxlen -= 2; h->in_chunk_hdr = 1; }
        }
        if (h->eof_body && h->used) { http_fail(h, "connessione chiusa durante il download"); } return;
      }
      if (h->rxlen) { size_t take = h->rxlen; if (h->clen >= 0 && (long long)take > h->clen) take = (size_t)h->clen; http_deliver(h, h->rx, take); if (!h->used) return; memmove(h->rx, h->rx + take, h->rxlen - take); h->rxlen -= take; if (h->clen >= 0) h->clen -= take; }
      if ((h->clen == 0) || (h->clen < 0 && h->eof_body)) { http_ev(h, "end", JS_UNDEFINED, JS_UNDEFINED); http_free(h); return; }
      if (h->clen > 0 && h->eof_body) { http_fail(h, "download incompleto"); return; }
      return;
    }
    return;
  }
}


// ---------- richieste verso il bridge JS (dalla app o da un socket di prova) ----------
static void report_exception(JSContext *ctx, const char *where); static void drain_jobs(Engine *e); static void flush_rejections(JSContext *ctx);
typedef struct Req {
  struct Req *next; int id, done, status, from_tcp, tcp_slot;
  char *method, *url, *headers; unsigned char *body; size_t blen;
  unsigned char *resp; size_t rlen, rcap; char *rheaders;
  pthread_cond_t cv;
} Req;
static pthread_mutex_t rq_mu = PTHREAD_MUTEX_INITIALIZER; static Req *rq_head, *rq_tail; static int rq_next_id; static Req *rq_active[64];
static JSValue rq_handler = JS_UNDEFINED;
static void req_free(Req *r) { free(r->method); free(r->url); free(r->headers); free(r->body); free(r->resp); free(r->rheaders); pthread_cond_destroy(&r->cv); free(r); }
static JSValue js_set_handler(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { JS_FreeValue(ctx, rq_handler); rq_handler = JS_DupValue(ctx, argv[0]); return JS_UNDEFINED; }
// respond(id, status, headersJson|null, chunk|null, last)
static JSValue js_respond(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int id = 0, status = 0, last = 0; JS_ToInt32(ctx, &id, argv[0]); JS_ToInt32(ctx, &status, argv[1]); last = JS_ToBool(ctx, argv[4]);
  Req *r = NULL; pthread_mutex_lock(&rq_mu); for (int i = 0; i < 64; i++) if (rq_active[i] && rq_active[i]->id == id) { r = rq_active[i]; break; } pthread_mutex_unlock(&rq_mu);
  if (!r) return JS_FALSE;
  if (!JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2])) { const char *hj = JS_ToCString(ctx, argv[2]); if (hj) { free(r->rheaders); r->rheaders = strdup(hj); r->status = status; JS_FreeCString(ctx, hj); } }
  if (!JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3])) { size_t n = 0; uint8_t *p = get_bytes(ctx, argv[3], &n); if (p && n) { if (r->rlen + n > r->rcap) { r->rcap = (r->rlen + n) * 2 + 4096; r->resp = realloc(r->resp, r->rcap); } memcpy(r->resp + r->rlen, p, n); r->rlen += n; } }
  if (last) { pthread_mutex_lock(&rq_mu); r->done = 1; for (int i = 0; i < 64; i++) if (rq_active[i] == r) rq_active[i] = NULL; if (!r->from_tcp) pthread_cond_signal(&r->cv); pthread_mutex_unlock(&rq_mu); }
  return JS_TRUE;
}
int eng_request(Engine *e, const char *method, const char *url, const char *headers_json, const void *body, size_t blen, int timeout_ms, EngResponse *out) {
  Req *r = calloc(1, sizeof *r); pthread_cond_init(&r->cv, NULL); r->method = strdup(method); r->url = strdup(url); r->headers = strdup(headers_json ? headers_json : "{}");
  if (blen) { r->body = malloc(blen); memcpy(r->body, body, blen); r->blen = blen; }
  memset(out, 0, sizeof *out);
  pthread_mutex_lock(&rq_mu); r->id = ++rq_next_id; r->next = NULL; if (rq_tail) rq_tail->next = r; else rq_head = r; rq_tail = r;
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += timeout_ms / 1000; ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  int rc = 0; while (!r->done && rc == 0) rc = pthread_cond_timedwait(&r->cv, &rq_mu, &ts);
  int ok = r->done;
  if (!ok) { for (int i = 0; i < 64; i++) if (rq_active[i] == r) rq_active[i] = NULL; r->done = -1; }   // scaduta: il motore la liberera' quando risponde
  pthread_mutex_unlock(&rq_mu);
  if (!ok) return -1;
  out->status = r->status ? r->status : 200; out->headers_json = r->rheaders; r->rheaders = NULL; out->body = r->resp; out->len = r->rlen; r->resp = NULL; req_free(r); return 0;
}
void eng_response_free(EngResponse *r) { free(r->headers_json); free(r->body); memset(r, 0, sizeof *r); }
// il motore prende le richieste in coda e le passa al gestore JS
static void rq_pump(Engine *e) {
  if (JS_IsUndefined(rq_handler)) return;
  for (int n = 0; n < 8; n++) {
    pthread_mutex_lock(&rq_mu); Req *r = rq_head; if (r) { rq_head = r->next; if (!rq_head) rq_tail = NULL; int placed = 0; for (int i = 0; i < 64; i++) if (!rq_active[i]) { rq_active[i] = r; placed = 1; break; } if (!placed) { r->next = rq_head; rq_head = r; if (!rq_tail) rq_tail = r; r = NULL; } } pthread_mutex_unlock(&rq_mu);
    if (!r) return;
    JSContext *ctx = e->ctx; JSValue hv = JS_ParseJSON(ctx, r->headers, strlen(r->headers), "<headers>"); if (JS_IsException(hv)) { JS_FreeValue(ctx, JS_GetException(ctx)); hv = JS_NewObject(ctx); }
    JSValue args[5] = { JS_NewInt32(ctx, r->id), JS_NewString(ctx, r->method), JS_NewString(ctx, r->url), hv, r->blen ? new_ab(ctx, r->body, r->blen) : JS_NULL };
    JSValue res = JS_Call(ctx, rq_handler, JS_UNDEFINED, 5, args); if (JS_IsException(res)) report_exception(ctx, "gestore richieste"); JS_FreeValue(ctx, res); for (int i = 0; i < 5; i++) JS_FreeValue(ctx, args[i]);
    drain_jobs(e);
  }
}


// ---------- server TCP locale (solo prove sul PC): stesso schema richieste/risposte ----------
typedef struct { int fd; unsigned char *buf; size_t len, cap; Req *req; } TcpConn;
static int tcp_lfd = -1; static TcpConn tcpc[16];
int eng_listen_tcp(Engine *e, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return -1; int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(0x7f000001);
  if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 8) != 0) { close(fd); return -1; }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); tcp_lfd = fd; return 0;
}
static void tcp_close(TcpConn *c) { if (c->fd >= 0) close(c->fd); free(c->buf); memset(c, 0, sizeof *c); c->fd = -1; }
static void tcp_send_all(int fd, const unsigned char *p, size_t n) { size_t off = 0; while (off < n) { ssize_t w = send(fd, p + off, n - off, 0); if (w > 0) { off += w; continue; } if (errno == EAGAIN || errno == EWOULDBLOCK) { fd_set fs; FD_ZERO(&fs); FD_SET(fd, &fs); struct timeval tv = { 1, 0 }; select(fd + 1, NULL, &fs, NULL, &tv); continue; } return; } }
static void tcp_service(Engine *e) {
  static int init; if (!init) { for (int i = 0; i < 16; i++) tcpc[i].fd = -1; init = 1; }
  if (tcp_lfd < 0) return;
  for (;;) { int cfd = accept(tcp_lfd, NULL, NULL); if (cfd < 0) break; int slot = -1; for (int i = 0; i < 16; i++) if (tcpc[i].fd < 0) { slot = i; break; } if (slot < 0) { close(cfd); continue; } fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL, 0) | O_NONBLOCK); memset(&tcpc[slot], 0, sizeof tcpc[slot]); tcpc[slot].fd = cfd; }
  for (int i = 0; i < 16; i++) {
    TcpConn *c = &tcpc[i]; if (c->fd < 0) continue;
    if (c->req) {   // in attesa della risposta del JS
      pthread_mutex_lock(&rq_mu); int done = c->req->done; pthread_mutex_unlock(&rq_mu);
      if (done) {
        Req *r = c->req; char head[4096]; int n = snprintf(head, sizeof head, "HTTP/1.1 %d OK\r\nContent-Length: %zu\r\nConnection: close\r\n", r->status ? r->status : 200, r->rlen);
        JSContext *ctx = e->ctx; if (r->rheaders) { JSValue ho = JS_ParseJSON(ctx, r->rheaders, strlen(r->rheaders), "<h>"); if (!JS_IsException(ho)) { JSPropertyEnum *tab; uint32_t cnt; if (JS_GetOwnPropertyNames(ctx, &tab, &cnt, ho, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) { for (uint32_t k = 0; k < cnt; k++) { const char *key = JS_AtomToCString(ctx, tab[k].atom); JSValue v = JS_GetProperty(ctx, ho, tab[k].atom); const char *vs = JS_ToCString(ctx, v); if (key && vs && strcasecmp(key, "content-length") && strcasecmp(key, "connection") && strcasecmp(key, "transfer-encoding") && n < (int)sizeof head - 300) n += snprintf(head + n, sizeof head - n, "%s: %s\r\n", key, vs); JS_FreeCString(ctx, vs); JS_FreeCString(ctx, key); JS_FreeValue(ctx, v); JS_FreeAtom(ctx, tab[k].atom); } js_free(ctx, tab); } } else JS_FreeValue(ctx, JS_GetException(ctx)); JS_FreeValue(ctx, ho); }
        n += snprintf(head + n, sizeof head - n, "\r\n"); tcp_send_all(c->fd, (unsigned char *)head, n); tcp_send_all(c->fd, r->resp, r->rlen); req_free(r); c->req = NULL; tcp_close(c);
      }
      continue;
    }
    if (c->len + 65536 > c->cap) { c->cap = c->cap ? c->cap * 2 : 131072; c->buf = realloc(c->buf, c->cap); }
    ssize_t r = recv(c->fd, c->buf + c->len, c->cap - c->len, 0);
    if (r > 0) c->len += r; else if (r == 0) { tcp_close(c); continue; } else if (errno != EAGAIN && errno != EWOULDBLOCK) { tcp_close(c); continue; }
    unsigned char *he = NULL; for (size_t k = 3; k < c->len; k++) if (c->buf[k] == '\n' && c->buf[k - 1] == '\r' && c->buf[k - 2] == '\n' && c->buf[k - 3] == '\r') { he = c->buf + k + 1; break; }
    if (!he) continue;
    size_t hl = he - c->buf; char *hdr = malloc(hl + 1); memcpy(hdr, c->buf, hl); hdr[hl] = 0; long long clen = 0;
    for (char *p = hdr; *p; p++) if ((p == hdr || p[-1] == '\n') && !strncasecmp(p, "content-length:", 15)) clen = atoll(p + 15);
    if (c->len < hl + (size_t)clen) { free(hdr); continue; }
    char method[16] = "", path[4096] = ""; sscanf(hdr, "%15s %4095s", method, path);
    // intestazioni -> JSON
    size_t jcap = hl * 2 + 64, jl = 0; char *js = malloc(jcap); js[jl++] = '{'; int first = 1; char *line = strstr(hdr, "\r\n"); line = line ? line + 2 : hdr + hl;
    while (*line && !(line[0] == '\r')) { char *eol = strstr(line, "\r\n"); if (!eol) break; *eol = 0; char *cl = strchr(line, ':'); if (cl) { *cl = 0; char *v = cl + 1; while (*v == ' ') v++; for (char *k = line; *k; k++) if (*k >= 'A' && *k <= 'Z') *k += 32; if (!first) js[jl++] = ','; first = 0; jl += snprintf(js + jl, jcap - jl, "\"%s\":\"", line); for (char *k = v; *k && jl + 8 < jcap; k++) { if (*k == '"' || *k == '\\') js[jl++] = '\\'; js[jl++] = *k; } js[jl++] = '"'; } line = eol + 2; }
    js[jl++] = '}'; js[jl] = 0;
    Req *rq = calloc(1, sizeof *rq); pthread_cond_init(&rq->cv, NULL); rq->method = strdup(method); rq->url = strdup(path); rq->headers = js; rq->from_tcp = 1; if (clen) { rq->body = malloc(clen); memcpy(rq->body, c->buf + hl, clen); rq->blen = clen; }
    pthread_mutex_lock(&rq_mu); rq->id = ++rq_next_id; if (rq_tail) rq_tail->next = rq; else rq_head = rq; rq_tail = rq; pthread_mutex_unlock(&rq_mu);
    c->req = rq; free(hdr);
  }
}

// ---------- ciclo eventi ----------
static void report_exception(JSContext *ctx, const char *where) {
  JSValue ex = JS_GetException(ctx); const char *s = JS_ToCString(ctx, ex); JSValue st = JS_GetPropertyStr(ctx, ex, "stack"); const char *ss = JS_IsUndefined(st) ? NULL : JS_ToCString(ctx, st);
  elog("ERRORE JS (%s): %s%s%s", where, s ? s : "?", ss ? "\n" : "", ss ? ss : ""); JS_FreeCString(ctx, s); JS_FreeCString(ctx, ss); JS_FreeValue(ctx, st); JS_FreeValue(ctx, ex);
}
static void drain_jobs(Engine *e) { JSContext *c1; int r; while ((r = JS_ExecutePendingJob(e->rt, &c1)) != 0) { if (r < 0) report_exception(c1, "job"); } }
int eng_loop_step(Engine *e, int max_wait_ms) {
  { static long long last_mem; long long t = now_ms(); if (t - last_mem > 30000) { last_mem = t; JSMemoryUsage mu; JS_ComputeMemoryUsage(e->rt, &mu); struct mallinfo mi = mallinfo(); elog("memoria motore: JS %.1f MB (%lld oggetti, %lld stringhe); heap totale in uso %.1f MB", mu.malloc_size / 1048576.0, (long long)mu.obj_count, (long long)mu.str_count, mi.uordblks / 1048576.0); } }
  drain_jobs(e); tcp_service(e); rq_pump(e); if (!JS_IsUndefined(rq_handler) && max_wait_ms > 10) max_wait_ms = 10;   // con il gestore attivo si controlla la coda spesso
  long long now = now_ms(); Timer *best = NULL; for (Timer *t = e->timers; t; t = t->next) if (!best || t->due < best->due) best = t;
  long long wait = max_wait_ms; if (best) { long long d = best->due - now; if (d < 0) d = 0; if (d < wait) wait = d; }
  int nfd = 0, maxfd = -1; fd_set rs; FD_ZERO(&rs); int pending_ssl = 0;
  for (int i = 0; i < MAXWS; i++) { WS *w = &e->ws[i]; if (!w->used) continue; nfd++; if (w->state == 2) pending_ssl = 1; else { FD_SET(w->net.fd, &rs); if (w->net.fd > maxfd) maxfd = w->net.fd; if (mbedtls_ssl_get_bytes_avail(&w->ssl) > 0) pending_ssl = 1; } }
  for (int i = 0; i < MAXHTTP; i++) { Http *h = &G_http[i]; if (!h->used) continue; nfd++; FD_SET(h->net.fd, &rs); if (h->net.fd > maxfd) maxfd = h->net.fd; if (h->tls && mbedtls_ssl_get_bytes_avail(&h->ssl) > 0) pending_ssl = 1; }
  if (tcp_lfd >= 0) { FD_SET(tcp_lfd, &rs); if (tcp_lfd > maxfd) maxfd = tcp_lfd; for (int i = 0; i < 16; i++) if (tcpc[i].fd >= 0) { FD_SET(tcpc[i].fd, &rs); if (tcpc[i].fd > maxfd) maxfd = tcpc[i].fd; } }
  if (pending_ssl) wait = 0;
  if (maxfd >= 0 || wait > 0) { struct timeval tv = { (long)(wait / 1000), (long)((wait % 1000) * 1000) }; select(maxfd + 1, maxfd >= 0 ? &rs : NULL, NULL, NULL, &tv); }
  for (int i = 0; i < MAXWS; i++) { WS *w = &e->ws[i]; if (w->used) { ws_service(w); drain_jobs(e); } }
  for (int i = 0; i < MAXHTTP; i++) { if (G_http[i].used) { http_service(&G_http[i]); drain_jobs(e); } }
  now = now_ms();
  for (;;) {
    Timer *due = NULL; for (Timer *t = e->timers; t; t = t->next) if (t->due <= now && (!due || t->due < due->due)) due = t; if (!due) break;
    JSValue fn = JS_DupValue(e->ctx, due->fn);
    if (due->every) due->due = now + due->every; else { for (Timer **pp = &e->timers; *pp; pp = &(*pp)->next) if (*pp == due) { *pp = due->next; break; } JS_FreeValue(e->ctx, due->fn); free(due); }
    JSValue r = JS_Call(e->ctx, fn, JS_UNDEFINED, 0, NULL); if (JS_IsException(r)) report_exception(e->ctx, "timer"); JS_FreeValue(e->ctx, r); JS_FreeValue(e->ctx, fn); drain_jobs(e);
    now = now_ms();
  }
  drain_jobs(e); flush_rejections(e->ctx); tcp_service(e); rq_pump(e); int work = e->timers != NULL || !JS_IsUndefined(rq_handler); for (int i = 0; i < MAXWS; i++) if (e->ws[i].used) work = 1; for (int i = 0; i < MAXHTTP; i++) if (G_http[i].used) work = 1;
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
// promesse rifiutate: se qualcuno aggancia un gestore subito dopo (come fa async/await) non e' un errore; si segnalano solo quelle rimaste senza gestore a fine giro
typedef struct { JSValue promise; char *msg; int used; } PendRej;
static PendRej pend_rej[32];
static void rejection_tracker(JSContext *ctx, JSValueConst promise, JSValueConst reason, bool is_handled, void *opaque) {
  if (is_handled) { for (int i = 0; i < 32; i++) if (pend_rej[i].used && JS_VALUE_GET_PTR(pend_rej[i].promise) == JS_VALUE_GET_PTR(promise)) { JS_FreeValue(ctx, pend_rej[i].promise); free(pend_rej[i].msg); pend_rej[i].used = 0; } return; }
  int slot = -1; for (int i = 0; i < 32; i++) if (!pend_rej[i].used) { slot = i; break; } if (slot < 0) return;
  const char *s = JS_ToCString(ctx, reason); JSValue st = JS_IsObject(reason) ? JS_GetPropertyStr(ctx, reason, "stack") : JS_UNDEFINED; const char *ss = JS_IsUndefined(st) ? NULL : JS_ToCString(ctx, st);
  char buf[3000]; snprintf(buf, sizeof buf, "PROMISE RIFIUTATA senza gestione: %s%s%s", s ? s : "?", ss ? "\n" : "", ss ? ss : ""); JS_FreeCString(ctx, s); JS_FreeCString(ctx, ss); JS_FreeValue(ctx, st);
  pend_rej[slot].promise = JS_DupValue(ctx, promise); pend_rej[slot].msg = strdup(buf); pend_rej[slot].used = 1;
}
static void flush_rejections(JSContext *ctx) { for (int i = 0; i < 32; i++) if (pend_rej[i].used) { elog("%s", pend_rej[i].msg); JS_FreeValue(ctx, pend_rej[i].promise); free(pend_rej[i].msg); pend_rej[i].used = 0; } }
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
  FN("fsRead", js_fs_read, 1); FN("fsWrite", js_fs_write, 2); FN("fsUnlink", js_fs_unlink, 1); FN("fsMkdir", js_fs_mkdir, 1); FN("fsExists", js_fs_exists, 1); FN("fsList", js_fs_list, 1); FN("fsStat", js_fs_stat, 1); FN("fsRename", js_fs_rename, 2); FN("fsAppend", js_fs_append, 2); FN("fsReadRange", js_fs_read_range, 3);
  JS_SetPropertyStr(ctx, nat, "inflate", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_zlib, "inflate", 1, JS_CFUNC_generic_magic, 0)); JS_SetPropertyStr(ctx, nat, "deflate", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_zlib, "deflate", 1, JS_CFUNC_generic_magic, 1));
  FN("curveKeypair", js_curve_keypair, 1); FN("curveShared", js_curve_shared, 2); FN("curveSign", js_curve_sign, 3); FN("curveVerify", js_curve_verify, 3);
  FN("audioToWav", js_audio_to_wav, 1); FN("videoPlayable", js_video_playable, 1); FN("fsCopy", js_fs_copy, 2);
  FN("imgConvert", js_img_convert, 7); FN("imgInfo", js_img_info, 1);
  FN("setHandler", js_set_handler, 1); FN("respond", js_respond, 5);
  FN("httpRequest", js_http_request, 5); FN("httpAbort", js_http_abort, 1);
  FN("wsOpen", js_ws_open, 3); FN("wsSend", js_ws_send, 3); FN("wsClose", js_ws_close, 1);
  JS_SetPropertyStr(ctx, nat, "dataDir", JS_NewString(ctx, e->datadir));
  JS_SetPropertyStr(ctx, g, "__nat", nat); JS_FreeValue(ctx, g);
  return e;
}
void eng_free(Engine *e) { if (!e) return; for (int i = 0; i < MAXWS; i++) ws_free(&e->ws[i]); for (int i = 0; i < MAXHTTP; i++) http_free(&G_http[i]); JS_FreeContext(e->ctx); JS_FreeRuntime(e->rt); free(e); }
