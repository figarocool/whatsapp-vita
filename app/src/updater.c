#include "updater.h"
#include "json.h"
#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <pthread.h>
#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef SAFE_BUILD
#include <psp2/promoterutil.h>
#include <zlib.h>
#include "fpkg_head.h"
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Dove stanno gli aggiornamenti: release GitHub di https://github.com/figarocool/whatsapp-vita
// Per le prove si puo' mettere un altro indirizzo (anche http://...) in ux0:data/whatsapp/update_url.txt
// ---------------------------------------------------------------------------------------------------------------------
#define MANIFEST_URL "https://github.com/figarocool/whatsapp-vita/releases/latest/download/whatsapp-vita-update.json"
#define URL_OVERRIDE_FILE "ux0:data/whatsapp/update_url.txt"
#define CORE_ASSET "whatsapp-vita.vpk"
#define TITLE_ID "WHAP00001"
#define HELPER_TITLE "WHAPUPD01"
#define UPD_DIR "ux0:/data/whatsapp/updates"
#define INSTALL_ROOT UPD_DIR "/core-install"
#define HELPER_ROOT UPD_DIR "/helper-install"
#define HELPER_VPK "app0:/assets/wa-updater.vpk"
#define HELPER_MARKER UPD_DIR "/.updater-installed"
#define VERSION_MARKER UPD_DIR "/core-install.tag"
#define FAILURE_FILE UPD_DIR "/.install-failed"
#define CORE_FILE UPD_DIR "/whatsapp-vita.vpk"
#define CORE_PART UPD_DIR "/.whatsapp-vita.vpk.part"
#define MANIFEST_LIMIT (64u * 1024u)
#define CORE_LIMIT (48u * 1024u * 1024u)
#define INSTALLED_ROOT "ux0:/app/" TITLE_ID
#define ZIP_ENTRY_LIMIT 4096u

static pthread_mutex_t um = PTHREAD_MUTEX_INITIALIZER;
static UpdInfo ui;
static volatile int u_cancel, u_thread_running;
static char u_manifest_url[400];
static int u_curl_ready;

static void set_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void set_msg(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); pthread_mutex_lock(&um); vsnprintf(ui.msg, sizeof ui.msg, fmt, ap); pthread_mutex_unlock(&um); va_end(ap);
}
static void set_state(UpdState s) { pthread_mutex_lock(&um); ui.state = s; pthread_mutex_unlock(&um); }
static void set_progress(unsigned long long done, unsigned long long total) { pthread_mutex_lock(&um); ui.done = done; ui.total = total; pthread_mutex_unlock(&um); }
static void utrace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void utrace(const char *fmt, ...) {   // registro degli aggiornamenti: ux0:data/whatsapp/update.log
  char line[220]; va_list ap; va_start(ap, fmt); int n = vsnprintf(line, sizeof line - 1, fmt, ap); va_end(ap);
  if (n <= 0) return; if (n > (int)sizeof line - 2) n = sizeof line - 2; line[n++] = '\n';
  sceIoMkdir("ux0:data", 0777); sceIoMkdir("ux0:data/whatsapp", 0777);
  SceUID f = sceIoOpen("ux0:data/whatsapp/update.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666); if (f >= 0) { sceIoWrite(f, line, n); sceIoClose(f); }
}

// ---------- versioni: "v1.2.3" o "1.2.3", con eventuale suffisso -beta; le build "dev" non vengono mai sostituite ----------
typedef struct { unsigned part[3]; const char *suffix; } Ver;
static int parse_ver(const char *s, Ver *v) {
  if (!s) return 0; if (*s == 'v' || *s == 'V') s++;
  for (int i = 0; i < 3; i++) {
    if (*s < '0' || *s > '9') return 0;
    unsigned val = 0; do { unsigned d = (unsigned)(*s - '0'); if (val > (999999u - d) / 10u) return 0; val = val * 10u + d; s++; } while (*s >= '0' && *s <= '9');
    v->part[i] = val; if (i < 2) { if (*s != '.') return 0; s++; }
  }
  if (*s && *s != '-' && *s != '+') return 0; v->suffix = s; return 1;
}
static int ver_is_newer(const char *cand, const char *installed) {
  Ver a, b; if (!parse_ver(cand, &a) || !parse_ver(installed, &b)) return 0;
  for (int i = 0; i < 3; i++) if (a.part[i] != b.part[i]) return a.part[i] > b.part[i];
  int ap = *a.suffix == '-', bp = *b.suffix == '-'; if (ap != bp) return !ap; if (!ap) return 0; return strcmp(a.suffix, b.suffix) > 0;
}

// ---------- file ----------
static int read_small(const char *path, char *out, size_t cap) {
  SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0); if (f < 0) return -1;
  int n = sceIoRead(f, out, cap - 1); sceIoClose(f); if (n < 0) return -1; out[n] = 0; out[strcspn(out, "\r\n")] = 0; return n;
}
static int write_small(const char *path, const char *s) {
  SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666); if (f < 0) return -1;
  int n = (int)strlen(s), w = sceIoWrite(f, s, n); sceIoClose(f); return w == n ? 0 : -1;
}
static int mkdirs(const char *path) {
  char buf[512]; size_t n = strlen(path); if (!n || n >= sizeof buf) return -1; memcpy(buf, path, n + 1);
  for (size_t i = 1; i < n; i++) if (buf[i] == '/') { buf[i] = 0; sceIoMkdir(buf, 0777); buf[i] = '/'; }
  SceIoStat st; return sceIoMkdir(buf, 0777) < 0 && sceIoGetstat(buf, &st) < 0 ? -1 : 0;
}
static void remove_tree(const char *path) {
  SceIoStat st; if (sceIoGetstat(path, &st) < 0) return;
  if (SCE_S_ISDIR(st.st_mode)) {
    int d = sceIoDopen(path);
    if (d >= 0) { SceIoDirent e; while (sceIoDread(d, &e) > 0) { if (!strcmp(e.d_name, ".") || !strcmp(e.d_name, "..")) continue; char c[512]; int n = snprintf(c, sizeof c, "%s/%s", path, e.d_name); if (n > 0 && (size_t)n < sizeof c) remove_tree(c); } sceIoDclose(d); }
    sceIoRmdir(path);
  } else sceIoRemove(path);
}
static int sha256_file(const char *path, char hex[65]) {
  SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0); if (f < 0) return -1;
  mbedtls_sha256_context c; mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0);
  static unsigned char buf[32768]; int n;
  while ((n = sceIoRead(f, buf, sizeof buf)) > 0) mbedtls_sha256_update(&c, buf, n);
  sceIoClose(f); unsigned char d[32]; mbedtls_sha256_finish(&c, d); mbedtls_sha256_free(&c);
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", d[i]); hex[64] = 0; return n < 0 ? -1 : 0;
}
static int valid_digest(const char *s) { if (strlen(s) != 64) return 0; for (int i = 0; i < 64; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return 0; return 1; }

// ---------- rete (libcurl + mbedTLS: il TLS di SceHttps sul 3.60 non regge GitHub) ----------
typedef struct { char *d; size_t n; } Mem;
static size_t mem_write(char *p, size_t s, size_t m, void *u) { Mem *b = u; size_t k = s * m; if (k > MANIFEST_LIMIT - b->n) return 0; memcpy(b->d + b->n, p, k); b->n += k; b->d[b->n] = 0; return k; }
typedef struct { int fd; unsigned long long bytes, limit; int failed; } FileSink;
static size_t file_write(char *p, size_t s, size_t m, void *u) {
  FileSink *f = u; size_t k = s * m; if (k > f->limit - f->bytes) { f->failed = 1; return 0; }
  size_t at = 0; while (at < k) { int w = sceIoWrite(f->fd, p + at, k - at); if (w <= 0) { f->failed = 1; return 0; } at += (size_t)w; }
  f->bytes += k; return k;
}
static int xfer(void *unused, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
  (void)unused; (void)ut; (void)un; if (u_cancel) return 1;
  pthread_mutex_lock(&um); ui.done = dn > 0 ? (unsigned long long)dn : 0; if (dt > 0) ui.total = (unsigned long long)dt; pthread_mutex_unlock(&um); return 0;
}
static int http_fetch(const char *url, size_t (*wr)(char *, size_t, size_t, void *), void *ctx, long *status_out, int progress) {
  if (!u_curl_ready) { curl_global_init(CURL_GLOBAL_DEFAULT); u_curl_ready = 1; }
  CURL *c = curl_easy_init(); if (!c) return -1;
  char err[CURL_ERROR_SIZE] = { 0 };
  int plain = !strncmp(url, "http://", 7);   // solo se l'indirizzo e' stato messo a mano per le prove
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err); curl_easy_setopt(c, CURLOPT_URL, url); curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr); curl_easy_setopt(c, CURLOPT_WRITEDATA, ctx);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L); curl_easy_setopt(c, CURLOPT_TIMEOUT, progress ? 900L : 60L); curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "WhatsAppVita/1 (PS Vita updater)");
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, plain ? "http,https" : "https"); curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, plain ? "http,https" : "https");
  curl_easy_setopt(c, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
  curl_easy_setopt(c, CURLOPT_CAINFO, "app0:/assets/cacert.pem"); curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L); curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  if (progress) { curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L); curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L); curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L); curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer); curl_easy_setopt(c, CURLOPT_XFERINFODATA, NULL); }
  CURLcode rc = curl_easy_perform(c); long st = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st); if (status_out) *status_out = st;
  if (rc != CURLE_OK) utrace("curl: errore %d (%s) http=%ld", (int)rc, err, st); else if (st != 200) utrace("curl: http=%ld", st);
  curl_easy_cleanup(c); return rc == CURLE_OK && st == 200 ? 0 : (u_cancel ? -2 : -1);
}
static void load_manifest_url(void) {
  char o[400]; if (read_small(URL_OVERRIDE_FILE, o, sizeof o) > 8 && (!strncmp(o, "http://", 7) || !strncmp(o, "https://", 8))) snprintf(u_manifest_url, sizeof u_manifest_url, "%s", o);
  else snprintf(u_manifest_url, sizeof u_manifest_url, "%s", MANIFEST_URL);
}
// indirizzo di un file della stessa release del manifest
static void asset_url(const char *tag, const char *asset, char *out, size_t cap) {
  const char *p = strstr(u_manifest_url, "/releases/latest/download/");
  if (p) snprintf(out, cap, "%.*s/releases/download/%s/%s", (int)(p - u_manifest_url), u_manifest_url, tag, asset);
  else { const char *s = strrchr(u_manifest_url, '/'); snprintf(out, cap, "%.*s/%s", (int)(s - u_manifest_url), u_manifest_url, asset); }
}

// ---------- manifest ----------
static char m_digest[72]; static unsigned long long m_size;
static int fetch_manifest(void) {
  static char buf[MANIFEST_LIMIT + 1]; Mem m = { buf, 0 }; buf[0] = 0; long st = 0;
  load_manifest_url(); utrace("controllo: %s (versione installata %s)", u_manifest_url, ui.current);
  char fresh[480]; snprintf(fresh, sizeof fresh, "%s%ct=%llu", u_manifest_url, strchr(u_manifest_url, '?') ? '&' : '?', (unsigned long long)sceKernelGetSystemTimeWide());   // GitHub tiene in cache l'alias 'latest' per qualche minuto
  int rc = http_fetch(fresh, mem_write, &m, &st, 0); if (rc < 0 || !m.n) return rc == -2 ? -2 : -1;
  jv *j = json_parse(buf); if (!j) return -1;
  int ok = 0;
  if ((int)json_num(j, "schema", 0) == 1) {
    const char *tag = json_str(j, "tag", ""); jv *core = json_get(j, "core");
    const char *asset = core ? json_str(core, "asset", "") : "", *sha = core ? json_str(core, "sha256", "") : "";
    if (tag[0] && strlen(tag) < sizeof ui.tag && !strcmp(asset, CORE_ASSET) && valid_digest(sha)) {
      pthread_mutex_lock(&um); snprintf(ui.tag, sizeof ui.tag, "%s", tag); snprintf(ui.notes, sizeof ui.notes, "%s", json_str(j, "notes", "")); pthread_mutex_unlock(&um);
      snprintf(m_digest, sizeof m_digest, "%s", sha); m_size = (unsigned long long)json_num(core, "size", 0); ok = 1;
    }
  }
  json_free(j); utrace("manifest %s: tag=%s", ok ? "ok" : "NON valido", ui.tag); return ok ? 0 : -1;
}

// ---------- thread di controllo ----------
static void *check_thread(void *arg) {
  int manual = (int)(intptr_t)arg;
  set_state(UPD_CHECKING); set_msg("Controllo aggiornamenti..."); pthread_mutex_lock(&um); ui.quiet = !manual; pthread_mutex_unlock(&um);
  int rc = fetch_manifest();
  if (rc < 0) { set_state(UPD_ERROR); set_msg("Impossibile controllare gli aggiornamenti (rete o GitHub)"); }
  else if (ver_is_newer(ui.tag, ui.current)) {
    if (!strcmp(ui.last_fail_tag, ui.tag) && ui.last_fail_code) { utrace("versione %s gia' fallita (0x%08X): non riprovo da solo", ui.tag, (unsigned)ui.last_fail_code); pthread_mutex_lock(&um); ui.quiet = !manual; pthread_mutex_unlock(&um); }
    set_state(UPD_AVAILABLE); set_msg("Nuova versione disponibile");
  } else { set_state(UPD_UPTODATE); set_msg("L'app e' aggiornata"); }
  u_thread_running = 0; return NULL;
}

// ---------- installazione (solo versione completa) ----------
#ifndef SAFE_BUILD
static int safe_zip_name(const char *n) {
  if (!n || !*n || n[0] == '/' || strchr(n, '\\') || strchr(n, ':') || strstr(n, "../")) return 0;
  const char *p = n; while (*p) { const char *sl = strchr(p, '/'); size_t len = sl ? (size_t)(sl - p) : strlen(p); if (len == 2 && p[0] == '.' && p[1] == '.') return 0; if (!len) return sl && !sl[1]; p = sl ? sl + 1 : p + len; }
  return 1;
}
static uint16_t zu16(const unsigned char *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t zu32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int zread_at(int fd, uint32_t off, void *data, size_t size) {
  if (sceIoLseek(fd, (SceOff)off, SCE_SEEK_SET) < 0) return -1; unsigned char *p = data; size_t done = 0;
  while (done < size) { int n = sceIoRead(fd, p + done, size - done); if (n <= 0) return -1; done += (size_t)n; } return 0;
}
static int zwrite_all(int fd, const unsigned char *d, size_t size) { size_t done = 0; while (done < size) { int n = sceIoWrite(fd, d + done, size - done); if (n <= 0) return -1; done += (size_t)n; } return 0; }
static int extract_entry(int src, int dst, uint32_t off, uint32_t comp, uint32_t exp_size, uint32_t exp_crc, uint16_t method) {
  static unsigned char in[16384], out[16384]; uint32_t remaining = comp, written = 0; uLong crc = crc32(0L, Z_NULL, 0);
  if (method == 0) {
    while (remaining) { size_t k = remaining < sizeof in ? remaining : sizeof in; if (zread_at(src, off, in, k) < 0 || zwrite_all(dst, in, k) < 0) return -1; crc = crc32(crc, in, (uInt)k); off += k; remaining -= k; written += k; }
  } else if (method == 8) {
    z_stream s; memset(&s, 0, sizeof s); if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return -1; int r = Z_OK;
    while (r == Z_OK) {
      if (!s.avail_in && remaining) { uInt k = remaining < sizeof in ? remaining : (uInt)sizeof in; if (zread_at(src, off, in, k) < 0) { r = Z_DATA_ERROR; break; } off += k; remaining -= k; s.next_in = in; s.avail_in = k; }
      s.next_out = out; s.avail_out = sizeof out; r = inflate(&s, Z_NO_FLUSH); size_t prod = sizeof out - s.avail_out;
      if (prod) { if (written > exp_size || prod > exp_size - written || zwrite_all(dst, out, prod) < 0) { r = Z_DATA_ERROR; break; } crc = crc32(crc, out, (uInt)prod); written += (uint32_t)prod; }
      if (r == Z_BUF_ERROR && !remaining && !s.avail_in) break;
    }
    inflateEnd(&s); if (r != Z_STREAM_END) return -1;
  } else return -1;
  return written == exp_size && (uint32_t)crc == exp_crc ? 0 : -1;
}
// Legge direttamente le voci ZIP/VPK (store e deflate), controllando dimensioni e CRC di ogni file.
static int extract_vpk(const char *vpk, const char *root, int report) {
  int zip = sceIoOpen(vpk, SCE_O_RDONLY, 0); if (zip < 0) return -1;
  SceOff end = sceIoLseek(zip, 0, SCE_SEEK_END); if (end < 22 || end > UINT32_MAX) { sceIoClose(zip); return -1; }
  uint32_t fsz = (uint32_t)end, tsz = fsz < 65557u ? fsz : 65557u, toff = fsz - tsz;
  unsigned char *tail = malloc(tsz); if (!tail || zread_at(zip, toff, tail, tsz) < 0) { free(tail); sceIoClose(zip); return -1; }
  int eocd = -1; for (int i = (int)tsz - 22; i >= 0; i--) if (zu32(tail + i) == 0x06054b50u && (uint32_t)i + 22u + (uint32_t)zu16(tail + i + 20) == tsz) { eocd = i; break; }
  if (eocd < 0) { free(tail); sceIoClose(zip); return -1; }
  uint16_t entries = zu16(tail + eocd + 10); uint32_t cd_size = zu32(tail + eocd + 12), cd_off = zu32(tail + eocd + 16); free(tail);
  if (entries > ZIP_ENTRY_LIMIT || cd_off > fsz || cd_size > fsz - cd_off) { sceIoClose(zip); return -1; }
  uint64_t total = 0; uint32_t cursor = cd_off; int ok = 1; char name[512], path[1024], parent[1024]; unsigned char hdr[46], loc[30];
  for (uint16_t i = 0; i < entries && ok; i++) {
    if (u_cancel) { ok = 0; break; }
    if (cursor > fsz - 46 || zread_at(zip, cursor, hdr, sizeof hdr) < 0 || zu32(hdr) != 0x02014b50u) { ok = 0; break; }
    uint16_t flags = zu16(hdr + 8), method = zu16(hdr + 10), nl = zu16(hdr + 28), el = zu16(hdr + 30), cl = zu16(hdr + 32);
    uint32_t crc = zu32(hdr + 16), comp = zu32(hdr + 20), unc = zu32(hdr + 24), lo = zu32(hdr + 42); uint64_t next = (uint64_t)cursor + 46 + nl + el + cl;
    if (!nl || nl >= sizeof name || next > (uint64_t)cd_off + cd_size || (flags & 1) || comp == UINT32_MAX || unc == UINT32_MAX || lo >= fsz || unc > CORE_LIMIT * 2 - total) { ok = 0; break; }
    if (zread_at(zip, cursor + 46, name, nl) < 0) { ok = 0; break; } name[nl] = 0;
    if (!safe_zip_name(name)) { ok = 0; break; } total += unc;
    int n = snprintf(path, sizeof path, "%s/%s", root, name); if (n < 0 || (size_t)n >= sizeof path) { ok = 0; break; }
    if (name[nl - 1] == '/') { if (mkdirs(path) < 0) ok = 0; }
    else {
      if (zread_at(zip, lo, loc, sizeof loc) < 0 || zu32(loc) != 0x04034b50u) { ok = 0; break; }
      uint64_t d64 = (uint64_t)lo + 30 + zu16(loc + 26) + zu16(loc + 28); if (d64 > fsz || comp > fsz - d64) { ok = 0; break; }
      memcpy(parent, path, (size_t)n + 1); char *sl = strrchr(parent, '/'); if (!sl) { ok = 0; break; } *sl = 0; if (mkdirs(parent) < 0) { ok = 0; break; }
      int out = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666); if (out < 0) { ok = 0; break; }
      ok = extract_entry(zip, out, (uint32_t)d64, comp, unc, crc, method) == 0; if (sceIoClose(out) < 0) ok = 0;
    }
    if (report) set_progress(i + 1, entries);
    cursor = (uint32_t)next;
  }
  if (cursor != (uint64_t)cd_off + cd_size) ok = 0;
  sceIoClose(zip); return ok ? 0 : -1;
}
static int file_has(const char *path, const char *needle) {
  static unsigned char d[8192]; int f = sceIoOpen(path, SCE_O_RDONLY, 0); if (f < 0) return 0; int n = sceIoRead(f, d, sizeof d); sceIoClose(f);
  size_t l = strlen(needle); for (int i = 0; i + (int)l <= n; i++) if (!memcmp(d + i, needle, l)) return 1; return 0;
}
// Moduli di sistema per installare un pacchetto (stesso schema di VitaShell)
static int load_paf(void) {
  uint32_t args[6] = { 0x180000u, UINT32_MAX, UINT32_MAX, 1u, UINT32_MAX, UINT32_MAX }; int result = -1;
  uint32_t opt[4] = { (uint32_t)sizeof opt, (uint32_t)&result, UINT32_MAX, UINT32_MAX };
  return sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof args, args, (SceSysmoduleOpt *)opt);
}
static int promote_directory(const char *path) {
  int res = load_paf(); if (res < 0) return res;
  res = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
  if (res < 0) { sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, NULL); return res; }
  res = scePromoterUtilityInit(); if (res >= 0) { res = scePromoterUtilityPromotePkgWithRif(path, 1); scePromoterUtilityExit(); }
  sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL); sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, NULL);
  return res;
}
// Prepara il pacchetto scaricato e avvia l'installatore. Ritorna 0 se l'installatore e' partito.
static int stage_and_launch(const char *vpk, const char *tag) {
  remove_tree(INSTALL_ROOT); if (mkdirs(INSTALL_ROOT) < 0) return -2;
  set_state(UPD_EXTRACTING); set_msg("Estraggo i file..."); set_progress(0, 1);
  if (extract_vpk(vpk, INSTALL_ROOT, 1) < 0) { remove_tree(INSTALL_ROOT); return u_cancel ? -20 : -3; }
  char ver[512], param[512], eboot[512], line[64]; SceIoStat st;
  snprintf(ver, sizeof ver, "%s/assets/version.txt", INSTALL_ROOT); snprintf(param, sizeof param, "%s/sce_sys/param.sfo", INSTALL_ROOT); snprintf(eboot, sizeof eboot, "%s/eboot.bin", INSTALL_ROOT);
  if (read_small(ver, line, sizeof line) < 0 || strcmp(line, tag) || !file_has(param, TITLE_ID)) { remove_tree(INSTALL_ROOT); return -4; }
  if (sceIoGetstat(eboot, &st) < 0 || st.st_size < 1024 || sceIoGetstat(INSTALLED_ROOT, &st) < 0) { remove_tree(INSTALL_ROOT); return -5; }
  if (write_small(VERSION_MARKER, tag) < 0) { remove_tree(INSTALL_ROOT); return -9; }
  // l'installatore e' un'app a parte (WHAPUPD01): solo un altro processo puo' sostituire l'app in esecuzione
  set_state(UPD_INSTALLING); set_msg("Preparo l'installatore..."); set_progress(0, 0);
  remove_tree(HELPER_ROOT);
  if (mkdirs(HELPER_ROOT) < 0 || extract_vpk(HELPER_VPK, HELPER_ROOT, 0) < 0) { remove_tree(HELPER_ROOT); return -6; }
  char hs[512], he[512]; snprintf(hs, sizeof hs, "%s/sce_sys/param.sfo", HELPER_ROOT); snprintf(he, sizeof he, "%s/eboot.bin", HELPER_ROOT);
  if (!file_has(hs, HELPER_TITLE)) { remove_tree(HELPER_ROOT); return -7; }
  if (sceIoGetstat(he, &st) < 0 || st.st_size < 1024) { remove_tree(HELPER_ROOT); return -8; }
  if (wa_make_head_bin(HELPER_ROOT, "app0:/assets/vitashell-head.bin", HELPER_TITLE) < 0) { remove_tree(HELPER_ROOT); return -10; }
  set_msg("Installo l'installatore...");
  int res = promote_directory(HELPER_ROOT); utrace("installatore: promozione -> 0x%08X", (unsigned)res); if (res < 0) return res;
  remove_tree(HELPER_ROOT); write_small(HELPER_MARKER, HELPER_TITLE);
  char uri[64]; snprintf(uri, sizeof uri, "psgm:play?titleid=%s", HELPER_TITLE);
  set_msg("Avvio l'installazione: l'app si riaprira' da sola"); res = sceAppMgrLaunchAppByUri(0xFFFFF, uri); utrace("installatore: avvio -> 0x%08X", (unsigned)res); if (res < 0) return res;
  pthread_mutex_lock(&um); ui.must_exit = 1; pthread_mutex_unlock(&um);
  return 0;
}
#endif

static const char *code_text(int c, char *b, size_t n) {
  switch (c) {
    case -1: snprintf(b, n, "Download non riuscito (rete o spazio)"); break;
    case -2: snprintf(b, n, "Impossibile creare le cartelle"); break;
    case -3: snprintf(b, n, "Il pacchetto scaricato e' danneggiato"); break;
    case -4: snprintf(b, n, "Il pacchetto non corrisponde alla versione"); break;
    case -5: snprintf(b, n, "Pacchetto incompleto"); break;
    case -6: case -7: case -8: snprintf(b, n, "Installatore mancante o non valido"); break;
    case -20: snprintf(b, n, "Aggiornamento annullato"); break;
    default: snprintf(b, n, "Installazione non riuscita (codice %08X)", (unsigned)c); break;
  }
  return b;
}

static void *install_thread(void *arg) {
  (void)arg; char url[512], hex[65], tag[48]; int rc = 0;
  pthread_mutex_lock(&um); snprintf(tag, sizeof tag, "%s", ui.tag); pthread_mutex_unlock(&um);
  u_cancel = 0; mkdirs(UPD_DIR);
  // 1) download (con la barra di avanzamento dell'interfaccia dell'app)
  set_state(UPD_DOWNLOADING); set_msg("Scarico l'aggiornamento..."); set_progress(0, m_size);
  asset_url(tag, CORE_ASSET, url, sizeof url);
  { char cur[65]; if (sha256_file(CORE_FILE, cur) == 0 && !strcasecmp(cur, m_digest)) utrace("pacchetto gia' scaricato"); else {
      sceIoRemove(CORE_PART); int fd = sceIoOpen(CORE_PART, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
      if (fd < 0) { rc = -2; goto fail; }
      FileSink fs = { fd, 0, CORE_LIMIT, 0 }; long st = 0; utrace("scarico %s", url);
      int r = http_fetch(url, file_write, &fs, &st, 1); sceIoClose(fd);
      if (r == -2) { sceIoRemove(CORE_PART); rc = -20; goto fail; }
      if (r < 0 || fs.failed) { sceIoRemove(CORE_PART); rc = -1; goto fail; }
      // 2) verifica
      set_state(UPD_VERIFYING); set_msg("Controllo la firma (SHA-256)..."); set_progress(0, 0);
      if (sha256_file(CORE_PART, hex) < 0 || strcasecmp(hex, m_digest)) { utrace("SHA-256 diverso: atteso %s, trovato %s", m_digest, hex); sceIoRemove(CORE_PART); rc = -3; goto fail; }
      sceIoRemove(CORE_FILE); if (sceIoRename(CORE_PART, CORE_FILE) < 0) { rc = -2; goto fail; }
  } }
#ifdef SAFE_BUILD
  rc = -30; set_state(UPD_ERROR); set_msg("Pacchetto scaricato in ux0:data/whatsapp/updates: installalo con VitaShell"); u_thread_running = 0; return NULL;
#else
  rc = stage_and_launch(CORE_FILE, tag);
  if (rc == 0) { u_thread_running = 0; return NULL; }
#endif
fail: {
    char b[160]; code_text(rc, b, sizeof b); utrace("aggiornamento fallito: %d", rc);
    set_state(UPD_ERROR); set_msg("%s", b); pthread_mutex_lock(&um); ui.quiet = 0; pthread_mutex_unlock(&um);
  }
  u_thread_running = 0; return NULL;
}

// ---------- API ----------
static int start_thread(void *(*fn)(void *), void *arg) {
  if (u_thread_running) return -1; u_thread_running = 1;
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 512 * 1024); pthread_t t;
  int r = pthread_create(&t, &at, fn, arg); pthread_attr_destroy(&at);
  if (r != 0) { u_thread_running = 0; return -1; } pthread_detach(t); return 0;
}
void upd_cancel(void) { u_cancel = 1; }
void upd_retry(void) { pthread_mutex_lock(&um); int ok = ui.tag[0] != 0; if (ok) { ui.state = UPD_AVAILABLE; ui.quiet = 0; } pthread_mutex_unlock(&um); if (ok) upd_install(); }
void upd_snapshot(UpdInfo *out) { pthread_mutex_lock(&um); *out = ui; pthread_mutex_unlock(&um); }
int upd_busy(void) { pthread_mutex_lock(&um); UpdState s = ui.state; pthread_mutex_unlock(&um); return s == UPD_DOWNLOADING || s == UPD_VERIFYING || s == UPD_EXTRACTING || s == UPD_INSTALLING; }
void upd_dismiss(void) { pthread_mutex_lock(&um); if (ui.state == UPD_ERROR || ui.state == UPD_UPTODATE) ui.state = UPD_IDLE; pthread_mutex_unlock(&um); }
void upd_check(int manual) { if (upd_busy()) return; if (start_thread(check_thread, (void *)(intptr_t)manual) < 0) utrace("controllo: thread gia' in corso"); }
void upd_install(void) {
#ifdef SAFE_BUILD
  // la versione sicura puo' solo scaricare il pacchetto
#endif
  UpdInfo s; upd_snapshot(&s); if (s.state != UPD_AVAILABLE) return;
  if (start_thread(install_thread, NULL) < 0) utrace("installazione: thread gia' in corso");
}
void upd_note(const char *s) { utrace("%s", s); }   // riga nel registro degli aggiornamenti (anche prima che parta la grafica)
void upd_init(void) {
  memset(&ui, 0, sizeof ui); ui.state = UPD_IDLE;
#ifdef SAFE_BUILD
  ui.can_install = 0;
#else
  ui.can_install = 1;
#endif
  if (read_small("app0:/assets/version.txt", ui.current, sizeof ui.current) <= 0) snprintf(ui.current, sizeof ui.current, "dev");
  // un aggiornamento appena riuscito: si tolgono pacchetto e cartella di installazione
  char tag[64];
  if (read_small(VERSION_MARKER, tag, sizeof tag) > 0 && !strcmp(tag, ui.current)) { remove_tree(INSTALL_ROOT); sceIoRemove(CORE_FILE); sceIoRemove(CORE_PART); sceIoRemove(VERSION_MARKER); sceIoRemove(FAILURE_FILE); utrace("aggiornamento a %s completato", ui.current); }
  char f[96]; if (read_small(FAILURE_FILE, f, sizeof f) > 0) { unsigned code = 0; char t[48] = { 0 }; if (sscanf(f, "%47s %x", t, &code) == 2) { snprintf(ui.last_fail_tag, sizeof ui.last_fail_tag, "%s", t); ui.last_fail_code = (int)code; } }
}
// L'installatore (WHAPUPD01) e' un'app a parte: una volta tornati in WhatsApp va cancellato (usa moduli di sistema: prima della grafica).
void upd_cleanup_helper(void) {
#ifndef SAFE_BUILD
  SceIoStat st; if (sceIoGetstat(HELPER_MARKER, &st) < 0) return;
  // L'installatore (che ci ha appena riaperto) potrebbe essere ancora in uscita: cancellarlo mentre gira faceva chiudere l'app. Si aspetta.
  utrace("pulizia installatore: attendo che sia uscito"); sceKernelDelayThread(4 * 1000 * 1000);
  utrace("pulizia installatore: inizio");
  int res = load_paf(); if (res < 0) { utrace("pulizia installatore: moduli non caricati 0x%08X", (unsigned)res); return; }
  res = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
  if (res >= 0) { if (scePromoterUtilityInit() >= 0) { scePromoterUtilityDeletePkg(HELPER_TITLE); scePromoterUtilityExit(); } sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL); }
  sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, NULL); sceIoRemove(HELPER_MARKER);
  utrace("pulizia installatore: fine");
#endif
}
