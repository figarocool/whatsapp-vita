#include "net.h"
#include "engine.h"
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

// ---- motore interno: gli indirizzi "engine://local/percorso" non passano dalla rete ma vanno direttamente al bridge JavaScript ----
static Engine *g_eng;
void net_set_engine(Engine *e) { g_eng = e; }
static int is_eng(const char *url) { return !strncmp(url, "engine://", 9); }
static const char *eng_path(const char *url) { const char *p = strchr(url + 9, '/'); return p ? p : "/"; }
// ritorna 0 se il motore ha risposto (anche con errore HTTP, in *status); -1 se non risponde
static int eng_call(const char *method, const char *url, const void *body, size_t blen, const char *ctype, long timeout_s, char **out, size_t *len, int *status) {
  if (out) *out = NULL; if (len) *len = 0; if (status) *status = 0; if (!g_eng) return -1;
  char hdr[200]; snprintf(hdr, sizeof hdr, "{\"content-type\":\"%s\",\"host\":\"127.0.0.1\"}", ctype ? ctype : "application/json");
  EngResponse r; if (eng_request(g_eng, method, eng_path(url), hdr, body, blen, (int)(timeout_s * 1000), &r) != 0) return -1;
  if (status) *status = r.status; if (out) { *out = malloc(r.len + 1); if (!*out) { eng_response_free(&r); return -1; } memcpy(*out, r.body, r.len); (*out)[r.len] = 0; } if (len) *len = r.len;
  eng_response_free(&r); return 0;
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
  if (is_eng(url)) { int st; if (eng_call("GET", url, NULL, 0, NULL, timeout_s, out, len, &st) != 0) return -1; if (st >= 400) { free(*out); *out = NULL; if (len) *len = 0; return -1; } return 0; }
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
  if (is_eng(url)) {
    char *b = NULL; size_t l = 0; int st; if (eng_call("GET", url, NULL, 0, NULL, timeout_s, &b, &l, &st) != 0 || st >= 400) { free(b); return -1; }
    SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (f < 0) { free(b); return -1; }
    size_t at = 0; while (at < l) { size_t k = l - at > 65536 ? 65536 : l - at; int w = sceIoWrite(f, b + at, k); if (w <= 0) { sceIoClose(f); sceIoRemove(path); free(b); return -1; } at += (size_t)w; if (cb && cb(at, l)) { sceIoClose(f); sceIoRemove(path); free(b); return -2; } }
    sceIoClose(f); free(b); return 0;
  }
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
  if (is_eng(url)) { int st = 0; int rc = eng_call("POST", url, body, strlen(body), "application/json", 90, NULL, NULL, &st); return rc != 0 || st >= 400 ? -1 : 0; }   // errore anche se il motore risponde 500
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
  if (is_eng(url)) { int st; if (eng_call("POST", url, data, len, ctype, 180, NULL, NULL, &st) != 0 || st >= 400) return -1; return 0; }
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
  if (is_eng(url)) { int st; if (eng_call("POST", url, body, strlen(body), "application/json", 30, out, NULL, &st) != 0 || st >= 400) { free(*out); *out = NULL; return -1; } return 0; }
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
  if (is_eng(url)) { char *b = NULL; int st; int ok = (eng_call("GET", url, NULL, 0, NULL, 2, &b, NULL, &st) == 0 && b && strstr(b, "\"status\"")) ? 0 : -1; free(b); return ok; }
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
