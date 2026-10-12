#include "lite.h"
#include "wabin_tokens.h"
#ifdef LITE_PLUGIN
// Plugin della shell: niente libc, mbedTLS ridotto, rete/tempo/memoria forniti da plat.h
#include "plat.h"
#include "wa_root.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/curve25519.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#define psa_crypto_init() 0
#define LITE_WAIT(c, usec, rd, wr) plat_wait_fd((c)->net.fd, (usec), (rd), (wr))
#define LITE_NOW() plat_now_ms()
#define LITE_STRERROR(rc, buf, n) snprintf(buf, n, "codice -0x%x", (unsigned)-(rc))
#else
#include "../src/curve25519.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/select.h>
#include <sys/time.h>
#include "mbedtls/ssl.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "mbedtls/error.h"
#include "psa/crypto.h"
static long long pc_now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static void pc_wait(int fd, int usec, int rd, int wr) { fd_set f; FD_ZERO(&f); FD_SET(fd, &f); struct timeval tv = { 0, usec }; select(fd + 1, rd ? &f : NULL, wr ? &f : NULL, NULL, &tv); }
#define LITE_WAIT(c, usec, rd, wr) pc_wait((c)->net.fd, (usec), (rd), (wr))
#define LITE_NOW() pc_now_ms()
#define LITE_STRERROR(rc, buf, n) mbedtls_strerror(rc, buf, n)
#endif

struct LiteClient {
  mbedtls_ssl_context ssl; mbedtls_ssl_config conf; mbedtls_net_context net; mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg; mbedtls_x509_crt ca; int ca_ok, up;
  uint8_t npriv[32], npub[32]; uint8_t *payload; size_t plen;
  unsigned char *wrx; size_t wlen, wcap;      // byte grezzi del WebSocket
  unsigned char *frag; size_t fraglen;        // messaggio WebSocket in ricostruzione
  unsigned char *nrx; size_t nlen, ncap;      // byte Noise (frame con lunghezza a 3 byte)
  uint8_t hash[32], salt[32], enck[32], deck[32]; uint32_t counter; int sent_intro, transport; uint32_t wr_ctr, rd_ctr; uint8_t t_enc[32], t_dec[32];
  long long last_tx, last_rx; int logged_in; int idc; char err[160];
};
static long long now_ms(void) { return LITE_NOW(); }
static int fail(LiteClient *c, int code, const char *m) { snprintf(c->err, sizeof c->err, "%s", m); return code; }
const char *lite_last_error(LiteClient *c) { return c->err; }

// ---------- crittografia Noise ----------
static void sha256b(uint8_t out[32], const uint8_t *a, size_t an, const uint8_t *b, size_t bn) { mbedtls_sha256_context s; mbedtls_sha256_init(&s); mbedtls_sha256_starts(&s, 0); mbedtls_sha256_update(&s, a, an); if (bn) mbedtls_sha256_update(&s, b, bn); mbedtls_sha256_finish(&s, out); mbedtls_sha256_free(&s); }
static void hmac256(uint8_t out[32], const uint8_t *key, size_t kn, const uint8_t *d1, size_t n1, const uint8_t *d2, size_t n2) { mbedtls_md_context_t m; mbedtls_md_init(&m); mbedtls_md_setup(&m, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1); mbedtls_md_hmac_starts(&m, key, kn); mbedtls_md_hmac_update(&m, d1, n1); if (n2) mbedtls_md_hmac_update(&m, d2, n2); mbedtls_md_hmac_finish(&m, out); mbedtls_md_free(&m); }
static void hkdf64(uint8_t out[64], const uint8_t *salt, const uint8_t *ikm, size_t n) { uint8_t prk[32], one = 1, two = 2; hmac256(prk, salt, 32, ikm, n, NULL, 0); hmac256(out, prk, 32, &one, 1, NULL, 0); hmac256(out + 32, prk, 32, out, 32, &two, 1); }
static int gcm(int enc, const uint8_t key[32], uint32_t ctr, const uint8_t *aad, size_t an, const uint8_t *in, size_t n, uint8_t *out) {   // enc: out = cifrato||tag(16); dec: in = cifrato||tag, out = chiaro
  uint8_t iv[12] = { 0 }; iv[8] = ctr >> 24; iv[9] = ctr >> 16; iv[10] = ctr >> 8; iv[11] = ctr; mbedtls_gcm_context g; mbedtls_gcm_init(&g); int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) { if (enc) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, iv, 12, aad, an, in, out, 16, out + n); else { if (n < 16) rc = -1; else rc = mbedtls_gcm_auth_decrypt(&g, n - 16, iv, 12, aad, an, in + n - 16, 16, in, out); } }
  mbedtls_gcm_free(&g); return rc;
}
static void authenticate(LiteClient *c, const uint8_t *d, size_t n) { if (!c->transport) sha256b(c->hash, c->hash, 32, d, n); }
static void mix(LiteClient *c, const uint8_t *d, size_t n) { uint8_t k[64]; hkdf64(k, c->salt, d, n); memcpy(c->salt, k, 32); memcpy(c->enck, k + 32, 32); memcpy(c->deck, k + 32, 32); c->counter = 0; }
static int hs_encrypt(LiteClient *c, const uint8_t *p, size_t n, uint8_t *out) { int rc = gcm(1, c->enck, c->counter++, c->hash, 32, p, n, out); if (rc == 0) authenticate(c, out, n + 16); return rc; }
static int hs_decrypt(LiteClient *c, const uint8_t *ct, size_t n, uint8_t *out) { int rc = gcm(0, c->deck, c->counter++, c->hash, 32, ct, n, out); if (rc == 0) authenticate(c, ct, n); return rc; }

// ---------- TLS + WebSocket ----------
static int tls_write_all(LiteClient *c, const unsigned char *p, size_t n) { size_t off = 0; long long t0 = now_ms(); while (off < n) { int r = mbedtls_ssl_write(&c->ssl, p + off, n - off); if (r > 0) { off += r; continue; } if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) { if (now_ms() - t0 > 15000) return -1; LITE_WAIT(c, 50000, r == MBEDTLS_ERR_SSL_WANT_READ, r == MBEDTLS_ERR_SSL_WANT_WRITE); continue; } return -1; } return 0; }
static int ws_send(LiteClient *c, int op, const uint8_t *d, size_t n) {
  uint8_t *f = malloc(n + 14); size_t h = 0; f[h++] = 0x80 | op; if (n < 126) f[h++] = 0x80 | (uint8_t)n; else if (n < 65536) { f[h++] = 0x80 | 126; f[h++] = n >> 8; f[h++] = n & 255; } else { f[h++] = 0x80 | 127; for (int i = 7; i >= 0; i--) f[h++] = (uint8_t)((unsigned long long)n >> (8 * i)); }
  uint8_t mk[4]; mbedtls_ctr_drbg_random(&c->drbg, mk, 4); memcpy(f + h, mk, 4); h += 4; for (size_t i = 0; i < n; i++) f[h + i] = d[i] ^ mk[i & 3]; int r = tls_write_all(c, f, h + n); free(f); if (r == 0) c->last_tx = now_ms(); return r;
}
// legge dal socket (non bloccante dopo l'upgrade); restituisce 1 se ha letto qualcosa, 0 niente, <0 chiusa
static int ws_fill(LiteClient *c) {
  if (c->wlen + 8192 > c->wcap) { c->wcap = c->wcap ? c->wcap * 2 : 32768; c->wrx = realloc(c->wrx, c->wcap); }
  int r = mbedtls_ssl_read(&c->ssl, c->wrx + c->wlen, c->wcap - c->wlen); if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0; if (r <= 0) return -1; c->wlen += r; c->last_rx = now_ms(); return 1;
}
// estrae un messaggio WebSocket completo (binary/text) -> malloc in *out; ritorna 1 ok, 0 incompleto, <0 chiusa
static int ws_next(LiteClient *c, uint8_t **out, size_t *outn) {
  size_t off = 0; int got = 0;
  while (c->wlen - off >= 2) {
    uint8_t *f = c->wrx + off; int op = f[0] & 15, fin = f[0] >> 7, mk = f[1] >> 7; unsigned long long len = f[1] & 127; size_t h = 2;
    if (len == 126) { if (c->wlen - off < 4) break; len = (f[2] << 8) | f[3]; h = 4; } else if (len == 127) { if (c->wlen - off < 10) break; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | f[2 + i]; h = 10; }
    uint8_t m4[4] = { 0 }; if (mk) { if (c->wlen - off < h + 4) break; memcpy(m4, f + h, 4); h += 4; }
    if (len > (8u << 20)) return -1; if (c->wlen - off < h + len) break;
    uint8_t *pl = f + h; if (mk) for (unsigned long long i = 0; i < len; i++) pl[i] ^= m4[i & 3];
    off += h + (size_t)len;
    if (op == 9) { ws_send(c, 10, pl, (size_t)len); }
    else if (op == 8) { memmove(c->wrx, c->wrx + off, c->wlen - off); c->wlen -= off; return -1; }
    else if (op == 1 || op == 2 || op == 0) { if (op != 0) c->fraglen = 0; c->frag = realloc(c->frag, c->fraglen + (size_t)len + 1); memcpy(c->frag + c->fraglen, pl, (size_t)len); c->fraglen += (size_t)len; if (fin) { *out = c->frag; *outn = c->fraglen; c->frag = NULL; c->fraglen = 0; got = 1; break; } }
  }
  if (off) { memmove(c->wrx, c->wrx + off, c->wlen - off); c->wlen -= off; } return got;
}
// ---------- protobuf minimo per l'handshake ----------
static size_t pb_bytes(uint8_t *o, int field, const uint8_t *d, size_t n) { size_t k = 0; o[k++] = (field << 3) | 2; if (n < 128) o[k++] = n; else { o[k++] = 0x80 | (n & 127); o[k++] = n >> 7; } memcpy(o + k, d, n); return k + n; }
static int pb_get(const uint8_t *p, size_t n, int field, const uint8_t **v, size_t *vn) {
  size_t i = 0; while (i < n) { uint64_t key = 0; int sh = 0; while (i < n) { uint8_t b = p[i++]; key |= (uint64_t)(b & 127) << sh; if (!(b & 128)) break; sh += 7; } int f = (int)(key >> 3), w = (int)(key & 7);
    if (w == 2) { uint64_t l = 0; sh = 0; while (i < n) { uint8_t b = p[i++]; l |= (uint64_t)(b & 127) << sh; if (!(b & 128)) break; sh += 7; } if (i + l > n) return 0; if (f == field) { *v = p + i; *vn = (size_t)l; return 1; } i += (size_t)l; }
    else if (w == 0) { while (i < n && (p[i++] & 128)) {} } else if (w == 1) i += 8; else if (w == 5) i += 4; else return 0; } return 0;
}
// ---------- frame Noise ----------
static int noise_send(LiteClient *c, const uint8_t *data, size_t n) {
  uint8_t *body = (uint8_t *)data; uint8_t *tmp = NULL; size_t bn = n;
  if (c->transport) { tmp = malloc(n + 16); if (gcm(1, c->t_enc, c->wr_ctr++, NULL, 0, data, n, tmp) != 0) { free(tmp); return -1; } body = tmp; bn = n + 16; }
  size_t intro = c->sent_intro ? 0 : 4; uint8_t *f = malloc(intro + 3 + bn); if (!c->sent_intro) { f[0] = 'W'; f[1] = 'A'; f[2] = 6; f[3] = 3; c->sent_intro = 1; } f[intro] = bn >> 16; f[intro + 1] = bn >> 8; f[intro + 2] = bn; memcpy(f + intro + 3, body, bn);
  int r = ws_send(c, 2, f, intro + 3 + bn); free(f); free(tmp); return r;
}
// prende il prossimo frame Noise dal buffer; ritorna 1 (con copia malloc), 0 incompleto
static int noise_next(LiteClient *c, uint8_t **out, size_t *outn) {
  if (c->nlen < 3) return 0; size_t sz = (c->nrx[0] << 16) | (c->nrx[1] << 8) | c->nrx[2]; if (c->nlen < sz + 3) return 0;
  *out = malloc(sz + 1); memcpy(*out, c->nrx + 3, sz); *outn = sz; memmove(c->nrx, c->nrx + 3 + sz, c->nlen - 3 - sz); c->nlen -= 3 + sz; return 1;
}
static void noise_feed(LiteClient *c, const uint8_t *d, size_t n) { if (c->nlen + n > c->ncap) { c->ncap = (c->nlen + n) * 2 + 4096; c->nrx = realloc(c->nrx, c->ncap); } memcpy(c->nrx + c->nlen, d, n); c->nlen += n; }
// ---------- nodi binari WhatsApp ----------
typedef struct { const uint8_t *b; size_t n, i; int err; } Rd;
static int rb(Rd *r) { if (r->i >= r->n) { r->err = 1; return 0; } return r->b[r->i++]; }
static int rint(Rd *r, int k) { int v = 0; for (int j = 0; j < k; j++) v = (v << 8) | rb(r); return v; }
static void rstr(Rd *r, int tag, char *out, size_t cap);
static void put(char *o, size_t cap, const char *s, size_t n) { size_t k = n < cap - 1 ? n : cap - 1; memcpy(o, s, k); o[k] = 0; }
static void rbin(Rd *r, size_t len, char *out, size_t cap) { if (r->i + len > r->n) { r->err = 1; out[0] = 0; return; } put(out, cap, (const char *)r->b + r->i, len); r->i += len; }
static void rpacked(Rd *r, int tag, char *out, size_t cap) {
  int st = rb(r); int cnt = st & 127; size_t k = 0; for (int i = 0; i < cnt && !r->err; i++) { int cb = rb(r); for (int h = 0; h < 2; h++) { int v = h ? (cb & 15) : (cb >> 4); char ch; if (tag == 255) ch = v < 10 ? '0' + v : v == 10 ? '-' : v == 11 ? '.' : '\0'; else ch = v < 10 ? '0' + v : 'A' + v - 10; if (k + 1 < cap) out[k++] = ch; } }
  if ((st >> 7) && k) k--; out[k] = 0;
}
static void rstr(Rd *r, int tag, char *out, size_t cap) {
  out[0] = 0;
  if (tag >= 1 && tag < WB_SINGLE_N) { put(out, cap, WB_SINGLE[tag], strlen(WB_SINGLE[tag])); return; }
  if (tag >= 236 && tag <= 239) { int d = tag - 236, ix = rb(r); if (ix < WB_DICT_N[d]) put(out, cap, WB_DICT[d][ix], strlen(WB_DICT[d][ix])); else r->err = 1; return; }
  switch (tag) {
    case 0: return;
    case 252: rbin(r, rb(r), out, cap); return;
    case 253: { int l = ((rb(r) & 15) << 16); l += rb(r) << 8; l += rb(r); rbin(r, l, out, cap); return; }
    case 254: rbin(r, rint(r, 4), out, cap); return;
    case 250: { char a[64], b[64]; rstr(r, rb(r), a, sizeof a); rstr(r, rb(r), b, sizeof b); snprintf(out, cap, "%s@%s", a, b); return; }
    case 247: { int dom = rb(r), dev = rb(r); char u[64]; rstr(r, rb(r), u, sizeof u); const char *srv = dom == 1 ? "lid" : dom == 128 ? "hosted" : dom == 129 ? "hosted.lid" : "s.whatsapp.net"; if (dev) snprintf(out, cap, "%s:%d@%s", u, dev, srv); else snprintf(out, cap, "%s@%s", u, srv); return; }
    case 246: { char u[64], s[32]; rstr(r, rb(r), u, sizeof u); int dev = rint(r, 2); rstr(r, rb(r), s, sizeof s); snprintf(out, cap, "%s:%d@%s", u, dev, s); return; }
    case 245: { char u[64], s[32]; rstr(r, rb(r), u, sizeof u); int dev = rint(r, 2), integ = rint(r, 2); size_t save = r->i; s[0] = 0; rstr(r, rb(r), s, sizeof s); if (r->err) { r->err = 0; r->i = save; strcpy(s, "interop"); } snprintf(out, cap, "%d-%s:%d@%s", integ, u, dev, s); return; }
    case 251: case 255: rpacked(r, tag, out, cap); return;
    default: r->err = 1; return;
  }
}
static int rlist_size(Rd *r, int tag) { return tag == 0 ? 0 : tag == 248 ? rb(r) : tag == 249 ? rint(r, 2) : (r->err = 1, 0); }
// decodifica solo il nodo di primo livello (tag e attributi): ai figli non serve
static int node_decode(const uint8_t *b, size_t n, LiteNode *o) {
  memset(o, 0, sizeof *o); if (n < 2) return -1; Rd r = { b + 1, n - 1, 0, 0 };
  if (b[0] & 2) return -2;   // compresso con zlib: non succede per i pacchetti che ci interessano
  int ls = rlist_size(&r, rb(&r)); char tag[64]; rstr(&r, rb(&r), tag, sizeof tag); if (r.err || !ls || !tag[0]) return -1; put(o->tag, sizeof o->tag, tag, strlen(tag));
  for (int i = 0; i < (ls - 1) >> 1 && !r.err; i++) {
    char k[40], v[200]; rstr(&r, rb(&r), k, sizeof k); rstr(&r, rb(&r), v, sizeof v); if (r.err) break;
    if (!strcmp(k, "from")) put(o->from, sizeof o->from, v, strlen(v)); else if (!strcmp(k, "participant")) put(o->participant, sizeof o->participant, v, strlen(v)); else if (!strcmp(k, "notify")) put(o->notify, sizeof o->notify, v, strlen(v));
    else if (!strcmp(k, "type")) put(o->type, sizeof o->type, v, strlen(v)); else if (!strcmp(k, "id")) put(o->id, sizeof o->id, v, strlen(v)); else if (!strcmp(k, "recipient")) put(o->recipient, sizeof o->recipient, v, strlen(v)); else if (!strcmp(k, "category")) put(o->category, sizeof o->category, v, strlen(v));
  }
  return r.err ? -1 : 0;
}
// scrittura di un nodo semplice: <tag a="b" ...><figlio/></tag>
typedef struct { uint8_t b[512]; size_t n; } Wr;
static void wpush(Wr *w, int v) { if (w->n < sizeof w->b) w->b[w->n++] = v; }
static void wstr(Wr *w, const char *s) {
  for (int i = 1; i < WB_SINGLE_N; i++) if (WB_SINGLE[i][0] && !strcmp(WB_SINGLE[i], s)) { wpush(w, i); return; }
  for (int d = 0; d < WB_DICTS; d++) for (int i = 0; i < WB_DICT_N[d]; i++) if (!strcmp(WB_DICT[d][i], s)) { wpush(w, 236 + d); wpush(w, i); return; }
  size_t l = strlen(s); wpush(w, 252); wpush(w, (int)l); for (size_t i = 0; i < l; i++) wpush(w, (uint8_t)s[i]);
}
static size_t node_encode(uint8_t *out, const char *tag, const char *attrs[][2], int nattr, const char *child) {
  Wr w; w.n = 0; wpush(&w, 0); wpush(&w, 248); wpush(&w, 2 * nattr + 1 + (child ? 1 : 0)); wstr(&w, tag);
  for (int i = 0; i < nattr; i++) { wstr(&w, attrs[i][0]); wstr(&w, attrs[i][1]); }
  if (child) { wpush(&w, 248); wpush(&w, 1); wpush(&w, 248); wpush(&w, 1); wstr(&w, child); }
  memcpy(out, w.b, w.n); return w.n;
}
static int send_node(LiteClient *c, const char *tag, const char *attrs[][2], int nattr, const char *child) { uint8_t buf[512]; size_t n = node_encode(buf, tag, attrs, nattr, child); return noise_send(c, buf, n); }
static int send_iq(LiteClient *c, const char *type, const char *xmlns, const char *child, const char *id) {
  char idb[24]; if (!id) { snprintf(idb, sizeof idb, "lite-%d", ++c->idc); id = idb; }
  const char *a[5][2] = { { "id", id }, { "to", "s.whatsapp.net" }, { "type", type }, { "xmlns", xmlns } }; return send_node(c, "iq", a, xmlns ? 4 : 3, child);
}
// ---------- collegamento ----------
LiteClient *lite_new(const uint8_t npriv[32], const uint8_t npub[32], const uint8_t *payload, size_t plen, const char *cacert) {
  LiteClient *c = calloc(1, sizeof *c); psa_crypto_init(); mbedtls_ssl_init(&c->ssl); mbedtls_ssl_config_init(&c->conf); mbedtls_net_init(&c->net); mbedtls_entropy_init(&c->ent); mbedtls_ctr_drbg_init(&c->drbg); mbedtls_x509_crt_init(&c->ca);
  mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->ent, (const unsigned char *)"wa-lite", 7); memcpy(c->npriv, npriv, 32); memcpy(c->npub, npub, 32); c->payload = malloc(plen); memcpy(c->payload, payload, plen); c->plen = plen;
#ifdef LITE_PLUGIN
  (void)cacert; if (mbedtls_x509_crt_parse_der(&c->ca, WA_ROOT_DER, sizeof WA_ROOT_DER) == 0) c->ca_ok = 1; return c;
#else
  if (cacert && mbedtls_x509_crt_parse_file(&c->ca, cacert) >= 0) c->ca_ok = 1; return c;
#endif
}
static void reset_conn(LiteClient *c) { if (c->up) { mbedtls_ssl_free(&c->ssl); mbedtls_net_free(&c->net); mbedtls_ssl_init(&c->ssl); mbedtls_net_init(&c->net); } c->up = 0; c->wlen = c->nlen = c->fraglen = 0; c->sent_intro = c->transport = 0; c->wr_ctr = c->rd_ctr = 0; c->logged_in = 0; }
void lite_close(LiteClient *c) { if (!c) return; if (c->up) { uint8_t cc[2] = { 3, 232 }; ws_send(c, 8, cc, 2); mbedtls_ssl_close_notify(&c->ssl); } reset_conn(c); free(c->wrx); free(c->frag); free(c->nrx); free(c->payload); mbedtls_ssl_config_free(&c->conf); mbedtls_x509_crt_free(&c->ca); mbedtls_ctr_drbg_free(&c->drbg); mbedtls_entropy_free(&c->ent); free(c); }
static int wait_ws(LiteClient *c, uint8_t **msg, size_t *mn, int timeout_ms) {   // attende il prossimo messaggio WebSocket
  long long t0 = now_ms();
  for (;;) { int r = ws_next(c, msg, mn); if (r != 0) return r; int f = ws_fill(c); if (f < 0) return -1; if (f == 0) { if (now_ms() - t0 > timeout_ms) return 0; if (mbedtls_ssl_get_bytes_avail(&c->ssl) == 0) LITE_WAIT(c, 100000, 1, 0); } }
}
int lite_connect(LiteClient *c) {
  reset_conn(c); int rc; char eb[120];
  if ((rc = mbedtls_net_connect(&c->net, "web.whatsapp.com", "443", MBEDTLS_NET_PROTO_TCP)) != 0) return fail(c, LITE_ERR_NET, "connessione TCP");
  c->up = 1; mbedtls_ssl_config_free(&c->conf); mbedtls_ssl_config_init(&c->conf);
  mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT); mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg); mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
  if (c->ca_ok) { mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL); mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED); } else mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_read_timeout(&c->conf, 15000); mbedtls_ssl_setup(&c->ssl, &c->conf); mbedtls_ssl_set_hostname(&c->ssl, "web.whatsapp.com"); mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);
  while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) { LITE_STRERROR(rc, eb, sizeof eb); snprintf(c->err, sizeof c->err, "TLS: %s", eb); return LITE_ERR_TLS; }
  uint8_t kr[16]; char key[32]; size_t kl; mbedtls_ctr_drbg_random(&c->drbg, kr, 16); mbedtls_base64_encode((unsigned char *)key, sizeof key, &kl, kr, 16); key[kl] = 0;
  char req[512]; int n = snprintf(req, sizeof req, "GET /ws/chat HTTP/1.1\r\nHost: web.whatsapp.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: https://web.whatsapp.com\r\n\r\n", key);
  if (tls_write_all(c, (unsigned char *)req, n) != 0) return fail(c, LITE_ERR_WS, "invio upgrade");
  char resp[1024]; size_t rl = 0; long long t0 = now_ms(); while (rl < sizeof resp - 1 && now_ms() - t0 < 15000) { int r = mbedtls_ssl_read(&c->ssl, (unsigned char *)resp + rl, 1); if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue; if (r <= 0) break; rl++; if (rl >= 4 && !memcmp(resp + rl - 4, "\r\n\r\n", 4)) break; }
  resp[rl] = 0; if (strncmp(resp, "HTTP/1.1 101", 12)) return fail(c, LITE_ERR_WS, "upgrade rifiutato");
  mbedtls_net_set_nonblock(&c->net); mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, NULL);
  // ---- handshake Noise XX ----
  static const char MODE[33] = "Noise_XX_25519_AESGCM_SHA256\0\0\0\0"; memcpy(c->hash, MODE, 32); memcpy(c->salt, c->hash, 32); memcpy(c->enck, c->hash, 32); memcpy(c->deck, c->hash, 32); c->counter = 0;
  static const uint8_t HDR[4] = { 'W', 'A', 6, 3 }; authenticate(c, HDR, 4);
  uint8_t seed[32], epub[32], epriv[32]; mbedtls_ctr_drbg_random(&c->drbg, seed, 32); c25519_keypair(seed, epub, epriv); authenticate(c, epub, 32);
  uint8_t hello[80], ch[100]; size_t hn = pb_bytes(hello, 1, epub, 32); size_t cn = pb_bytes(ch, 2, hello, hn);   // HandshakeMessage{ clientHello{ ephemeral } }
  if (noise_send(c, ch, cn) != 0) return fail(c, LITE_ERR_NOISE, "invio ClientHello");
  uint8_t *msg = NULL; size_t mn = 0; if (wait_ws(c, &msg, &mn, 15000) <= 0) return fail(c, LITE_ERR_NOISE, "nessun ServerHello");
  noise_feed(c, msg, mn); free(msg); uint8_t *fr = NULL; size_t fn = 0; if (!noise_next(c, &fr, &fn)) { return fail(c, LITE_ERR_NOISE, "ServerHello incompleto"); }
  const uint8_t *sh; size_t shn; const uint8_t *se, *ss, *sp; size_t sen, ssn, spn;
  if (!pb_get(fr, fn, 3, &sh, &shn) || !pb_get(sh, shn, 1, &se, &sen) || !pb_get(sh, shn, 2, &ss, &ssn) || !pb_get(sh, shn, 3, &sp, &spn) || sen != 32 || ssn < 48) { free(fr); return fail(c, LITE_ERR_NOISE, "ServerHello non valido"); }
  uint8_t dh[32]; authenticate(c, se, 32); c25519_shared(dh, epriv, se); mix(c, dh, 32);
  uint8_t sstatic[64]; if (hs_decrypt(c, ss, ssn, sstatic) != 0) { free(fr); return fail(c, LITE_ERR_NOISE, "chiave statica del server"); }
  c25519_shared(dh, epriv, sstatic); mix(c, dh, 32);
  uint8_t *cert = malloc(spn); if (hs_decrypt(c, sp, spn, cert) != 0) { free(cert); free(fr); return fail(c, LITE_ERR_NOISE, "certificato del server"); } free(cert);   // il certificato non si verifica: il collegamento e' gia' protetto da TLS
  uint8_t keyenc[48]; hs_encrypt(c, c->npub, 32, keyenc); c25519_shared(dh, c->npriv, se); mix(c, dh, 32);
  uint8_t *penc = malloc(c->plen + 16); hs_encrypt(c, c->payload, c->plen, penc);
  uint8_t *fin = malloc(c->plen + 128), *inner = malloc(c->plen + 100); size_t in = pb_bytes(inner, 1, keyenc, 48); in += pb_bytes(inner + in, 2, penc, c->plen + 16); size_t fnn = pb_bytes(fin, 4, inner, in);
  free(fr); free(penc); int sr = noise_send(c, fin, fnn); free(fin); free(inner); if (sr != 0) return fail(c, LITE_ERR_NOISE, "invio ClientFinish");
  { uint8_t k[64]; hkdf64(k, c->salt, (const uint8_t *)"", 0); memcpy(c->t_enc, k, 32); memcpy(c->t_dec, k + 32, 32); c->transport = 1; c->wr_ctr = c->rd_ctr = 0; }
  c->last_tx = c->last_rx = now_ms(); return LITE_OK;
}
int lite_poll(LiteClient *c, int timeout_ms, LiteOnNode cb, void *ud) {
  long long t0 = now_ms();
  for (;;) {
    uint8_t *m; size_t mn; int r;
    while ((r = ws_next(c, &m, &mn)) > 0) { noise_feed(c, m, mn); free(m); }
    if (r < 0) return LITE_CLOSED;
    uint8_t *fr; size_t fn;
    while (noise_next(c, &fr, &fn)) {
      uint8_t *pt = malloc(fn); int dr = gcm(0, c->t_dec, c->rd_ctr++, NULL, 0, fr, fn, pt); free(fr);
      if (dr != 0) { free(pt); return fail(c, LITE_ERR_NOISE, "decifratura del pacchetto"); }
      LiteNode nd; if (node_decode(pt, fn - 16, &nd) == 0) {
        if (!strcmp(nd.tag, "success")) { c->logged_in = 1; send_iq(c, "set", "passive", "active", NULL); }
        else if (!strcmp(nd.tag, "failure")) { free(pt); snprintf(c->err, sizeof c->err, "login rifiutato (%s)", nd.type); return LITE_ERR_LOGIN; }
        else if (!strcmp(nd.tag, "stream:error")) { free(pt); return LITE_CONFLICT; }
        else if (!strcmp(nd.tag, "iq") && !strcmp(nd.type, "get") && nd.id[0]) send_iq(c, "result", NULL, NULL, nd.id);
        if (cb) cb(&nd, ud);
      }
      free(pt);
    }
    long long now = now_ms();
    if (c->logged_in && now - c->last_tx > 25000) send_iq(c, "get", "w:p", "ping", NULL);
    if (now - c->last_rx > 90000) return LITE_CLOSED;
    if (now - t0 >= timeout_ms) return LITE_OK;
    int f = ws_fill(c); if (f < 0) return LITE_CLOSED;
    if (f == 0) { if (mbedtls_ssl_get_bytes_avail(&c->ssl) == 0) LITE_WAIT(c, 100000, 1, 0); }
  }
}
