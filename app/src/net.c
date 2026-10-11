#include "net.h"
#include <curl/curl.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { char *d; size_t n; } Buf;
static size_t wr(void *p, size_t s, size_t m, void *u) {
  Buf *b = u; size_t k = s * m;
  char *nd = realloc(b->d, b->n + k + 1); if (!nd) return 0;
  b->d = nd; memcpy(b->d + b->n, p, k); b->n += k; b->d[b->n] = 0; return k;
}
int net_start(void) {
  sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  static char pool[1024 * 1024];
  SceNetInitParam ip = { pool, sizeof pool, 0 };
  sceNetInit(&ip);
  sceNetCtlInit();
  return curl_global_init(CURL_GLOBAL_DEFAULT);
}
int http_get(const char *url, char **out, size_t *len) { return http_get_t(url, out, len, 15); }
int http_get_t(const char *url, char **out, size_t *len, long timeout_s) {
  *out = NULL; if (len) *len = 0;   // in caso di errore il chiamante trova NULL, mai un puntatore vecchio
  CURL *c = curl_easy_init(); if (!c) return -1;
  Buf b = { 0 };
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 6L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  CURLcode r = curl_easy_perform(c);
  curl_easy_cleanup(c);
  if (r != CURLE_OK) { free(b.d); return -1; }
  if (!b.d) { b.d = calloc(1, 1); }
  *out = b.d; if (len) *len = b.n; return 0;
}
typedef struct { int fd; int failed; dl_progress_fn cb; } DlCtx;
static size_t dl_write(void *p, size_t s, size_t m, void *u) {
  DlCtx *c = u; size_t k = s * m, at = 0;
  while (at < k) { int w = sceIoWrite(c->fd, (const char *)p + at, k - at); if (w <= 0) { c->failed = 1; return 0; } at += (size_t)w; }
  return k;
}
static int dl_xfer(void *u, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) { (void)ut; (void)un; DlCtx *c = u; return c->cb ? c->cb(dn > 0 ? (unsigned long long)dn : 0, dt > 0 ? (unsigned long long)dt : 0) : 0; }
// scarica direttamente su file (senza tenere tutto in memoria) e riporta l'avanzamento: serve ai video e ai file grandi
int http_download_file(const char *url, const char *path, long timeout_s, dl_progress_fn cb) {
  CURL *c = curl_easy_init(); if (!c) return -1;
  DlCtx ctx = { sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777), 0, cb }; if (ctx.fd < 0) { curl_easy_cleanup(c); return -1; }
  curl_easy_setopt(c, CURLOPT_URL, url); curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dl_write); curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s); curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L); curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L); curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L); curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dl_xfer); curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
  CURLcode r = curl_easy_perform(c); curl_easy_cleanup(c); sceIoClose(ctx.fd);
  if (r == CURLE_ABORTED_BY_CALLBACK) { sceIoRemove(path); return -2; }
  if (r != CURLE_OK || ctx.failed) { sceIoRemove(path); return -1; }
  return 0;
}
int http_post_json(const char *url, const char *body) {
  CURL *c = curl_easy_init(); if (!c) return -1;
  Buf b = { 0 };
  struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/json");
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
  CURLcode r = curl_easy_perform(c);
  curl_slist_free_all(h); curl_easy_cleanup(c); free(b.d);
  return r == CURLE_OK ? 0 : -1;
}

int http_post_buf(const char *url, const void *data, size_t len, const char *ctype) {
  CURL *c = curl_easy_init(); if (!c) return -1;
  Buf b = { 0 }; char hdr[96]; snprintf(hdr, sizeof hdr, "Content-Type: %s", ctype);
  struct curl_slist *h = curl_slist_append(NULL, hdr);
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, data);
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 180L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  CURLcode r = curl_easy_perform(c);
  curl_slist_free_all(h); curl_easy_cleanup(c); free(b.d);
  return r == CURLE_OK ? 0 : -1;
}

int http_post_json_resp(const char *url, const char *body, char **out) {
  *out = NULL;
  CURL *c = curl_easy_init(); if (!c) return -1;
  Buf b = { 0 };
  struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/json");
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  CURLcode r = curl_easy_perform(c);
  curl_slist_free_all(h); curl_easy_cleanup(c);
  if (r != CURLE_OK) { free(b.d); return -1; }
  *out = b.d; return 0;
}

// controlla velocemente se url risponde con un JSON del bridge (usato per cercare il PC in rete)
int http_probe(const char *url) {
  CURL *c = curl_easy_init(); if (!c) return -1;
  Buf b = { 0 };
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 150L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 900L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  CURLcode r = curl_easy_perform(c);
  curl_easy_cleanup(c);
  int ok = (r == CURLE_OK && b.d && strstr(b.d, "\"status\"")) ? 0 : -1;
  free(b.d); return ok;
}
