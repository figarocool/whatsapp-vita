// WhatsApp per PS Vita - front-end del bridge (vedi ../bridge). Tema scuro stile WhatsApp.
#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/rtc.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/net/netctl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <jpeglib.h>
#include "updater.h"
#include "player.h"
#include "engine.h"
#include "audio.h"
#include "i18n.h"
#include <psp2/camera.h>
#include <psp2/kernel/sysmem.h>
#include "json.h"
#include "net.h"

int _newlib_heap_size_user = 170 * 1024 * 1024;

// ---- scala dell'interfaccia: testo, foto, avatar e distanze crescono insieme ----
// Il codice di disegno ragiona in "unita' logiche"; ogni funzione grafica le moltiplica per UK (scala attuale).
static float UK = 1.0f;          // scala in uso (cambia da schermata a schermata e per i popup)
static float UKB = 1.0f;         // scala di base della schermata (UK si riduce temporaneamente per barre e popup)
static float ui_k = 2.0f;        // preferenza dell'utente: 1.0 normale, 1.5 grande, 2.0 molto grande
static unsigned ui_gen = 1;      // cambia ad ogni cambio di scala: invalida le impaginazioni salvate
static int rnd(float v) { return (int)floorf(v + 0.5f); }
static void vx_rect(float x, float y, float w, float h, unsigned c) { int x0 = rnd(x * UK), y0 = rnd(y * UK), x1 = rnd((x + w) * UK), y1 = rnd((y + h) * UK); vita2d_draw_rectangle(x0, y0, x1 - x0, y1 - y0, c); }
static void vx_circle(float x, float y, float r, unsigned c) { vita2d_draw_fill_circle(rnd(x * UK), rnd(y * UK), r * UK, c); }
static void vx_clip(int x0, int y0, int x1, int y1) {   // ritaglio sempre valido: valori fuori schermo o invertiti possono mandare in crash la GPU
  int a = rnd(x0 * UK), b = rnd(y0 * UK), c = rnd(x1 * UK), d = rnd(y1 * UK);
  if (a < 0) a = 0; if (b < 0) b = 0; if (c > 960) c = 960; if (d > 544) d = 544; if (c <= a) c = a + 1; if (d <= b) d = b + 1;
  vita2d_set_clip_rectangle(a, b, c, d);
}
static void vx_tex(vita2d_texture *t, float x, float y, float sx, float sy) { vita2d_draw_texture_scale(t, rnd(x * UK), rnd(y * UK), sx * UK, sy * UK); }
static void vx_tex_part(vita2d_texture *t, float x, float y, float tx, float ty, float tw_, float th_, float sx, float sy) { vita2d_draw_texture_part_scale(t, rnd(x * UK), rnd(y * UK), tx, ty, tw_, th_, sx * UK, sy * UK); }
#define vita2d_draw_rectangle vx_rect
#define vita2d_draw_fill_circle vx_circle
#define vita2d_set_clip_rectangle vx_clip
#define vita2d_draw_texture_scale vx_tex
#define vita2d_draw_texture_part_scale vx_tex_part
#define SW ((int)(960.0f / UK))
#define SH ((int)(544.0f / UK))
// barre fisse (intestazione, filtri, schede, barra di scrittura): crescono al massimo a 1,3x, il contenuto cresce di piu'
#define KCH (fminf(UKB, 1.3f))
#define HDR   ((int)(60.0f * KCH / UK + 0.5f))
#define CHIPS ((int)(46.0f * KCH / UK + 0.5f))
#define TABH  ((int)(56.0f * KCH / UK + 0.5f))
#define BARH  ((int)(64.0f * KCH / UK + 0.5f))
#define EMO_PANEL ((int)(240.0f / UK))
#define CHROME_BEGIN() float _uk_sv = UK; UK = KCH; clip_y0 = 0; clip_y1 = SH;
#define CHROME_END() UK = _uk_sv;

// ---- tema (WhatsApp dark) ----
// colori dell'interfaccia: variabili, cambiano con il tema (predefinito: chiaro)
static unsigned C_BG, C_BAR, C_TEXT, C_SUB, C_GREEN, C_GREEN2, C_OUT, C_IN, C_DIV, C_CHIP, C_CHIPSEL, C_BLUE, C_QUOTE, C_RED;
#define C_WHITE   RGBA8(255, 255, 255, 255)
static int ui_dark;     // 0 = chiaro (predefinito), 1 = scuro
static void apply_ui_theme(int dark) {
  ui_dark = dark;
  if (dark) {
    C_BG = RGBA8(0x0B, 0x14, 0x1A, 255); C_BAR = RGBA8(0x1F, 0x2C, 0x34, 255); C_TEXT = RGBA8(0xE9, 0xED, 0xEF, 255); C_SUB = RGBA8(0x86, 0x96, 0xA0, 255);
    C_GREEN = RGBA8(0x00, 0xA8, 0x84, 255); C_GREEN2 = RGBA8(0x25, 0xD3, 0x66, 255); C_OUT = RGBA8(0x00, 0x5C, 0x4B, 255); C_IN = RGBA8(0x20, 0x2C, 0x33, 255);
    C_DIV = RGBA8(0x22, 0x2D, 0x34, 255); C_CHIP = RGBA8(0x20, 0x2C, 0x33, 255); C_CHIPSEL = RGBA8(0x10, 0x3D, 0x32, 255); C_BLUE = RGBA8(0x53, 0xBD, 0xEB, 255);
    C_QUOTE = RGBA8(0x00, 0x00, 0x00, 70); C_RED = RGBA8(0xF1, 0x5C, 0x6D, 255);
  } else {
    C_BG = RGBA8(0xFF, 0xFF, 0xFF, 255); C_BAR = RGBA8(0xF0, 0xF2, 0xF5, 255); C_TEXT = RGBA8(0x11, 0x1B, 0x21, 255); C_SUB = RGBA8(0x66, 0x77, 0x81, 255);
    C_GREEN = RGBA8(0x00, 0xA8, 0x84, 255); C_GREEN2 = RGBA8(0x00, 0x80, 0x69, 255); C_OUT = RGBA8(0xD9, 0xFD, 0xD3, 255); C_IN = RGBA8(0xFF, 0xFF, 0xFF, 255);
    C_DIV = RGBA8(0xE2, 0xE6, 0xE9, 255); C_CHIP = RGBA8(0xF0, 0xF2, 0xF5, 255); C_CHIPSEL = RGBA8(0xD9, 0xFD, 0xD3, 255); C_BLUE = RGBA8(0x34, 0xB7, 0xF1, 255);
    C_QUOTE = RGBA8(0x00, 0x00, 0x00, 22); C_RED = RGBA8(0xEA, 0x00, 0x38, 255);
  }
}

static vita2d_font *font;                       // istanza principale (compatibilita')
static vita2d_font *font_override;             // se impostato, etext usa questo font (serve a scrivere il nome di ogni lingua con il suo font)
static vita2d_font *fonts[4];                   // quattro cache di caratteri separate (ognuna ha il suo atlante 512x512): piccolo / medio / grande / titoli
static int quant_size(int p) { static const int q[] = { 18, 22, 26, 30, 34, 40, 46, 54, 62, 72 }; int best = q[0]; for (int i = 0; i < 10; i++) if (abs(p - q[i]) < abs(p - best)) best = q[i]; return best; }   // poche dimensioni diverse: la cache non si riempie
static vita2d_font *font_for(int psize) { return psize <= 24 ? fonts[0] : psize <= 32 ? fonts[1] : psize <= 42 ? fonts[2] : fonts[3]; }
// larghezze dei testi memorizzate: misurare un testo costa (FreeType), e prima lo si faceva molte volte per frame
typedef struct { unsigned long long key; int w; } WCache; static WCache wcache[4096];
static int fw(vita2d_font *f, int psize, const char *s) {
  unsigned long long h = 1469598103934665603ULL; for (const unsigned char *p = (const unsigned char *)s; *p; p++) { h ^= *p; h *= 1099511628211ULL; } h ^= ((unsigned long long)psize << 56) | 1ULL;
  unsigned i = (unsigned)(h & 4095); if (wcache[i].key == h) return wcache[i].w;
  int w = vita2d_font_text_width(f, psize, s); wcache[i].key = h; wcache[i].w = w; return w;
}
static void crumb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// font dell'interfaccia: Roboto per le lingue latine e cirilliche, un font dedicato per giapponese, coreano e cinese
static char font_path_loaded[64]; static int font_loaded_once; static int cjk_ok;   // cjk_ok: con un font asiatico attivo si disegnano anche gli ideogrammi
#define ROBOTO_PATH "app0:assets/Roboto-Regular.ttf"
static const char *font_wanted(void) { const char *p = i18n_font_path(i18n_lang()); return p ? p : ROBOTO_PATH; }
static void load_fonts(void) {
  const char *p = font_wanted(); vita2d_font *nf[5];
  for (int i = 0; i < 5; i++) { nf[i] = vita2d_load_font_file(p); if (!nf[i]) { crumb("font NON caricato: %s (uso Roboto)", p); nf[i] = vita2d_load_font_file(ROBOTO_PATH); } }
  crumb("font caricato: %s", p);
  if (font_loaded_once) { vita2d_wait_rendering_done(); vita2d_free_font(font); for (int i = 0; i < 4; i++) vita2d_free_font(fonts[i]); }
  font = nf[0]; for (int i = 0; i < 4; i++) fonts[i] = nf[i + 1];
  memset(wcache, 0, sizeof wcache); snprintf(font_path_loaded, sizeof font_path_loaded, "%s", p); font_loaded_once = 1; cjk_ok = i18n_font_path(i18n_lang()) != NULL;
}
// texture da liberare: mai in mezzo al disegno, ma al frame successivo (la GPU potrebbe ancora usarle)
static vita2d_texture *dead_tex[96]; static int ndead;
static void tex_retire(vita2d_texture *t) { if (!t) return; if (ndead < 96) dead_tex[ndead++] = t; else { vita2d_wait_rendering_done(); vita2d_free_texture(t); } }
static void tex_reap(void) { if (!ndead) return; vita2d_wait_rendering_done(); for (int i = 0; i < ndead; i++) vita2d_free_texture(dead_tex[i]); ndead = 0; }
static unsigned pool_total, pool_max_used;      // memoria temporanea per frame della GPU: massimo usato

// ---- registro dei passaggi (per capire dove va in crash l'app) ----
#define LOGFILE "ux0:data/whatsapp/log.txt"
#define LOCKFILE "ux0:data/whatsapp/running.lock"
static pthread_mutex_t logmu = PTHREAD_MUTEX_INITIALIZER;
static char crash_lines[6][64]; static int crash_n, crash_pending;
static void crumb(const char *fmt, ...) {
  char b[190]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  pthread_mutex_lock(&logmu);
  SceUID f = sceIoOpen(LOGFILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  if (f >= 0) { char l[240]; int n = snprintf(l, sizeof l, "[%u] %s\n", (unsigned)(sceKernelGetProcessTimeLow() / 1000), b); sceIoWrite(f, l, n); sceIoClose(f); }
  pthread_mutex_unlock(&logmu);
}
void player_log(const char *s) { crumb("%s", s); }
static vita2d_texture *emo_tex;

// ======================================================================
// stato condiviso con il worker di rete
// ======================================================================
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char base_url[256] = "engine://local";   // predefinito: motore WhatsApp interno (nessun PC); un indirizzo http:// in config.txt usa invece un bridge sul PC
static char wa_status[24] = "starting";
static int net_ok = 0;
static char *qr_png; static size_t qr_len;
static jv *pend_chats; static jv *pend_msgs; static char pend_msgs_jid[160];
static jv *pend_prof;
static jv *pend_q; static char pend_q_tag[24];
static jv *qres; static char qtag[24], qwant[24]; static int qwait; static unsigned q_frame;
static jv *qres2; static char qtag2[24], qwant2[24];     // seconda area: le mie storie e chi le ha viste
static char open_jid[160];
static char pres_state[24], pres_jid[160];
static char toast_txt[160]; static int toast_new;
static volatile int quit_flag, poll_now;
static int sync_active, sync_pct, sync_chats, eng_busy_s; static unsigned sync_t0;
static volatile int scan_pct = -1;
static int list_sel, nav_mode, list_sel_dirty, dpad_hold;   // selezione con i tasti direzionali nella lista chat
static volatile int quit_req;
static volatile int chat_limit = 40, msg_limit = 30;   // caricamento a blocchi
static int chat_logged_frames, older_pending, no_more_older; static char anchor_id[64]; static unsigned msgs_gen, gen_req;

enum { J_SEND = 1, J_MEDIA, J_REACT, J_DELETE, J_PROFILE, J_PHOTO, J_DOWNLOAD, J_READ, J_QUERY, J_EXPORT, J_PREP, J_VPREP };
typedef struct { int type; char jid[160]; char id[64]; char text[2100]; char path[300]; char kind[12]; char extra[64]; } Job;
// trasferimenti con barra di avanzamento (video, audio, file): il thread di rete scrive, l'interfaccia disegna
static struct { volatile int active, cancel; char title[80], phase[96]; volatile unsigned long long done, total; } xfer;
static void xfer_phase(const char *ph) { pthread_mutex_lock(&mu); snprintf(xfer.phase, sizeof xfer.phase, "%s", ph); pthread_mutex_unlock(&mu); }
static int xfer_dl_cb(unsigned long long done, unsigned long long total) { xfer.done = done; xfer.total = total; return xfer.cancel; }
// anteprima video in-app (storie e chat): il bridge converte, l'app scarica in cache e lo riproduce con SceAvPlayer
static struct { char key[260]; char path[300]; volatile int gen, state, pct, phase, opened, audio, retries; volatile unsigned retry_at; } vp;
static void aud_toggle(const char *jid, const char *id); static int aud_state(const char *jid, const char *id, int *pct, int *secs);   // audio in chat (in extra.inc)   // state: 0 niente, 1 preparo, 2 pronto, 3 errore; phase: 0 WhatsApp, 1 conversione, 2 download
static __thread int vp_job_gen;
static int vp_dl_cb(unsigned long long done, unsigned long long total) { vp.pct = total ? (int)(100 * done / total) : 0; return vp_job_gen != vp.gen; }   // annulla se nel frattempo si e' passati ad altro
static void vp_cachefile(const char *id, int audio, char *out, size_t n) { char s[48]; snprintf(s, sizeof s, "%.40s", id); for (char *p = s; *p; p++) if (*p == '/' || *p == ':' || *p == '\\' || *p == '=') *p = '_'; snprintf(out, n, "ux0:data/whatsapp/cache/V7_%s.%s", s, audio ? "m4a" : "mp4"); }
// la cache dei video/audio resta fra un avvio e l'altro: si cancella solo il piu' vecchio quando supera 150 MB
static void vp_cache_wipe(void) {
  for (int round = 0; round < 200; round++) {
    SceUID d = sceIoDopen("ux0:data/whatsapp/cache"); if (d < 0) return; SceIoDirent e; long long tot = 0; char oldest[300] = ""; unsigned long long ot = ~0ULL;
    while (sceIoDread(d, &e) > 0) { if (e.d_name[0] != 'V') continue; tot += e.d_stat.st_size; unsigned long long t = (unsigned long long)e.d_stat.st_mtime.year * 400ULL * 86400 + (unsigned long long)e.d_stat.st_mtime.month * 32 * 86400 + (unsigned long long)e.d_stat.st_mtime.day * 86400 + e.d_stat.st_mtime.hour * 3600 + e.d_stat.st_mtime.minute * 60 + e.d_stat.st_mtime.second; if (t < ot) { ot = t; snprintf(oldest, sizeof oldest, "ux0:data/whatsapp/cache/%s", e.d_name); } }
    sceIoDclose(d); if (tot <= 150LL * 1024 * 1024 || !oldest[0]) return; sceIoRemove(oldest);
  }
}
#define MAXJOBS 8
static Job jobs[MAXJOBS]; static int njobs;

static Job *job_new(int type, const char *jid) {
  pthread_mutex_lock(&mu);
  if (njobs >= MAXJOBS) { pthread_mutex_unlock(&mu); return NULL; }
  Job *j = &jobs[njobs]; memset(j, 0, sizeof *j); j->type = type;
  if (jid) snprintf(j->jid, sizeof j->jid, "%s", jid);
  return j; // mutex resta preso fino a job_commit
}
static void job_commit(void) { njobs++; poll_now = 1; pthread_mutex_unlock(&mu); }

static void toast_set(const char *t) { pthread_mutex_lock(&mu); snprintf(toast_txt, sizeof toast_txt, "%s", t); toast_new = 1; pthread_mutex_unlock(&mu); }

// avatar e media (raw scaricato dal worker, decodificato dal main)
typedef struct { char jid[160]; int state; unsigned char *raw; size_t rawlen; vita2d_texture *tex; } Avatar; // 0 da chiedere,1 raw pronto,2 fatto,3 in corso
#define MAXAV 160
static Avatar avs[MAXAV]; static int nav;
static Avatar *av_find(const char *jid, int create) {
  for (int i = 0; i < nav; i++) if (!strcmp(avs[i].jid, jid)) return &avs[i];
  if (!create || nav >= MAXAV) return NULL;
  Avatar *a = &avs[nav++]; memset(a, 0, sizeof *a); snprintf(a->jid, sizeof a->jid, "%s", jid); return a;
}
typedef struct { char key[240]; char jid[160]; char id[64]; int state; unsigned char *raw; size_t rawlen; vita2d_texture *tex; unsigned last; int bstate; unsigned char *braw; size_t brawlen; vita2d_texture *big; } Media; // 0 da chiedere
#define MAXMED 24
static Media med[MAXMED]; static int nmed; static unsigned frame_no;

static void url_enc(const char *s, char *o, size_t n) {
  size_t k = 0;
  for (; *s && k + 4 < n; s++) {
    unsigned char c = *s;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.') o[k++] = c;
    else k += snprintf(o + k, n - k, "%%%02X", c);
  }
  o[k] = 0;
}
static void jesc(const char *s, char *o, size_t n) {
  size_t k = 0;
  for (; *s && k + 8 < n; s++) {
    if (*s == '"' || *s == '\\') { o[k++] = '\\'; o[k++] = *s; }
    else if (*s == '\n') { o[k++] = '\\'; o[k++] = 'n'; }
    else if ((unsigned char)*s >= 0x20) o[k++] = *s;
  }
  o[k] = 0;
}
static unsigned char *read_file(const char *path, size_t *len) {
  SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0); if (f < 0) return NULL;
  long sz = sceIoLseek(f, 0, SCE_SEEK_END); sceIoLseek(f, 0, SCE_SEEK_SET);
  if (sz <= 0 || sz > 30L * 1024 * 1024) { sceIoClose(f); return NULL; }
  unsigned char *b = malloc(sz + 1); if (!b) { sceIoClose(f); return NULL; }   // +1: il buffer e' sempre terminato da uno zero, cosi' si puo' usare anche come testo
  long got = 0; while (got < sz) { int r = sceIoRead(f, b + got, sz - got); if (r <= 0) break; got += r; }
  sceIoClose(f); if (got != sz) { free(b); return NULL; }
  b[sz] = 0; *len = sz; return b;
}
// eta' di un file in secondi (-1 se non esiste)
static int file_age_sec(const char *path) {
  SceIoStat st; if (sceIoGetstat(path, &st) < 0) return -1;
  SceDateTime now; sceRtcGetCurrentClockLocalTime(&now); SceRtcTick a, b; sceRtcGetTick(&now, &a); sceRtcGetTick(&st.st_mtime, &b);
  if (a.tick < b.tick) return 0; return (int)((a.tick - b.tick) / 1000000ULL);
}
// Stato del plugin delle notifiche: si guarda solo se il file di stato CAMBIA (il plugin lo riscrive ogni ~15 s).
// Non si confrontano orari: la Vita salva le date dei file in un fuso diverso dall'ora locale, e la differenza fa sembrare il plugin fermo.
static unsigned alive_chg_frame, alive_first_frame; static int alive_seen_any;
static void alive_poll(void) {
  static SceDateTime last; static int have;
  if (frame_no % 30) return;
  SceIoStat st; if (sceIoGetstat("ur0:tai/wa_alive.txt", &st) < 0) { have = 0; return; }
  if (!alive_first_frame) alive_first_frame = frame_no;
  if (have && memcmp(&last, &st.st_mtime, sizeof last) != 0) { alive_chg_frame = frame_no; alive_seen_any = 1; }
  last = st.st_mtime; have = 1;
}
// 1 = attivo, 0 = non attivo, -1 = verifica in corso (servono fino a ~40 s per vedere il file cambiare)
static int alive_state(void) {
  if (alive_seen_any && frame_no - alive_chg_frame < 45 * 60) return 1;
  if (!alive_first_frame || frame_no - alive_first_frame < 45 * 60) return -1;
  return 0;
}
static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); const char *c = strrchr(p, ':'); if (c && (!s || c > s)) s = c; return s ? s + 1 : p; }

static void run_job(Job *j, const char *base) {
  char url[1400], e1[500], e2[500], e3[300], e4[300];
  crumb("job %d %s", j->type, j->path);
  switch (j->type) {
    case J_SEND: {
      char t[4300], body[4800];
      jesc(j->text, t, sizeof t);
      snprintf(body, sizeof body, "{\"jid\":\"%s\",\"text\":\"%s\",\"quote\":\"%s\",\"edit\":\"%s\"}", j->jid, t, j->id, j->extra);
      snprintf(url, sizeof url, "%s/send", base);
      if (http_post_json(url, body)) toast_set(T("Invio non riuscito"));
      break;
    }
    case J_MEDIA: {
      size_t len; unsigned char *buf = read_file(j->path, &len);
      if (!buf) { toast_set(T("File non leggibile (max 30 MB)")); break; }
      url_enc(j->jid, e1, sizeof e1); url_enc(base_name(j->path), e2, sizeof e2); url_enc(j->text, e3, sizeof e3); url_enc(j->id, e4, sizeof e4);
      if (!strcmp(j->kind, "status")) snprintf(url, sizeof url, "%s/status/postmedia?caption=%s", base, e3);
      else snprintf(url, sizeof url, "%s/sendmedia?jid=%s&kind=%s&name=%s&caption=%s&quote=%s", base, e1, j->kind, e2, e3, e4);
      if (http_post_buf(url, buf, len, "application/octet-stream")) toast_set(T("Invio allegato non riuscito")); else toast_set(T("Allegato inviato"));
      free(buf); break;
    }
    case J_REACT: case J_DELETE: {
      char t[200], body[600];
      jesc(j->text, t, sizeof t);
      snprintf(body, sizeof body, "{\"jid\":\"%s\",\"id\":\"%s\",\"emoji\":\"%s\"}", j->jid, j->id, t);
      snprintf(url, sizeof url, "%s/%s", base, j->type == J_REACT ? "react" : "delete");
      if (http_post_json(url, body)) toast_set(T("Operazione non riuscita"));
      break;
    }
    case J_PROFILE: {
      char t[700], body[1200]; jesc(j->text, t, sizeof t);
      snprintf(body, sizeof body, "{\"%s\":\"%s\"}", j->kind, t);
      snprintf(url, sizeof url, "%s/profile", base);
      if (http_post_json(url, body)) toast_set(T("Profilo non aggiornato")); else toast_set(T("Profilo aggiornato"));
      break;
    }
    case J_PHOTO: {
      size_t len; unsigned char *buf = read_file(j->path, &len);
      if (!buf) { toast_set(T("Immagine non leggibile")); break; }
      snprintf(url, sizeof url, "%s/profile/photo", base);
      if (http_post_buf(url, buf, len, "application/octet-stream")) toast_set(T("Foto non aggiornata"));
      else { toast_set(T("Foto profilo aggiornata")); pthread_mutex_lock(&mu); Avatar *a = av_find(j->jid, 0); if (a && a->state != 3) a->state = 0; pthread_mutex_unlock(&mu); }
      free(buf); break;
    }
    case J_DOWNLOAD: {
      char *buf = NULL; size_t len = 0; char e[500], e2b[200];
      url_enc(j->jid, e, sizeof e); url_enc(j->id, e2b, sizeof e2b);
      int isvid = !strcmp(j->kind, "vid");
      snprintf(url, sizeof url, "%s/%s/%s/%s", base, isvid ? "vita-video" : "file", e, e2b);   // i video passano dal bridge che li converte per la Vita
      toast_set(isvid ? T("Converto il video per la Vita (puo' volerci un minuto)...") : T("Download in corso..."));
      if (http_get_t(url, &buf, &len, isvid ? 420 : 60)) { toast_set(isvid ? T("Video non disponibile (scaduto su WhatsApp o conversione fallita)") : T("Download non riuscito (file scaduto?)")); break; }
      char fn[300]; int pic = !strcmp(j->kind, "pic"), vid = !strcmp(j->kind, "vid");
      if (pic || vid) {   // foto e video ricevuti: salvati dove li trovi dal sistema
        const char *top = pic ? "ux0:picture" : "ux0:video"; char dir[64]; snprintf(dir, sizeof dir, "%s/WhatsApp", top); sceIoMkdir(top, 0777); sceIoMkdir(dir, 0777);
        const char *ext = vid ? "mp4" : (len > 8 && (unsigned char)buf[0] == 0x89 && buf[1] == 'P' ? "png" : "jpg");
        snprintf(fn, sizeof fn, "%s/WA_%.20s.%s", dir, j->id, ext); for (char *p = fn + strlen(dir) + 1; *p; p++) if (*p == '/' || *p == ':' || *p == '\\' || *p == '=') *p = '_';
      } else {
        sceIoMkdir("ux0:data/whatsapp/downloads", 0777);
        snprintf(fn, sizeof fn, "ux0:data/whatsapp/downloads/%s", j->text);
        for (char *p = fn + 28; *p; p++) if (*p == '/' || *p == ':' || *p == '\\') *p = '_';
      }
      SceUID f = sceIoOpen(fn, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
      if (f >= 0) { size_t w = 0; while (w < len) { int r = sceIoWrite(f, buf + w, len - w); if (r <= 0) break; w += r; } sceIoClose(f); if (pic) toast_set(T("Foto salvata in ux0:picture/WhatsApp")); else if (vid) toast_set(T("Video convertito e salvato in ux0:video/WhatsApp (apri l'app Video)")); else toast_set(T("Salvato in ux0:data/whatsapp/downloads")); }
      else toast_set(T("Impossibile salvare il file"));
      free(buf); break;
    }
    case J_PREP: {   // video, audio o file: il bridge li prepara (scarica da WhatsApp, converte per la Vita) con una percentuale; poi si scaricano con la barra
      int isvid = !strcmp(j->kind, "video"), isaud = !strcmp(j->kind, "audio");
      pthread_mutex_lock(&mu); snprintf(xfer.title, sizeof xfer.title, "%s", isvid ? T("Preparo il video") : isaud ? T("Preparo l'audio") : T("Scarico il file")); xfer.phase[0] = 0; xfer.done = xfer.total = 0; xfer.cancel = 0; xfer.active = 1; pthread_mutex_unlock(&mu);
      const char *fail = isvid ? T("Video non disponibile (scaduto su WhatsApp o conversione fallita)") : T("Download non riuscito (file scaduto?)");
      char e[500], e2b[200], key[64] = ""; url_enc(j->jid, e, sizeof e); url_enc(j->id, e2b, sizeof e2b);
      xfer_phase(T("Scarico da WhatsApp"));
      snprintf(url, sizeof url, "%s/prep/%s/%s?kind=%s", base, e, e2b, j->kind);
      { char *r = NULL; size_t rl = 0; int rc = http_get_t(url, &r, &rl, 20); jv *v = (rc == 0 && r) ? json_parse(r) : NULL; free(r); if (v) { snprintf(key, sizeof key, "%s", json_str(v, "key", "")); json_free(v); } }
      if (!key[0]) { toast_set(fail); xfer.active = 0; break; }
      int ready = 0, cancelled = 0; unsigned long long t0 = sceKernelGetProcessTimeWide();
      while (!ready && !cancelled && !quit_flag) {
        if (xfer.cancel) { cancelled = 1; break; }
        if (sceKernelGetProcessTimeWide() - t0 > 900ULL * 1000000ULL) break;
        snprintf(url, sizeof url, "%s/prep-status/%s", base, key); char *r = NULL; size_t rl = 0;
        if (http_get_t(url, &r, &rl, 10) == 0 && r) {
          jv *v = json_parse(r); if (v) {
            const char *st = json_str(v, "state", "");
            if (!strcmp(st, "ready")) ready = 1;
            else if (!strcmp(st, "error")) { json_free(v); free(r); toast_set(fail); xfer.active = 0; goto prep_done; }
            else if (!strcmp(st, "convert")) { xfer_phase(T("Converto per la Vita")); xfer.done = (unsigned long long)json_num(v, "pct", 0); xfer.total = 100; }
            else { xfer_phase(T("Scarico da WhatsApp")); xfer.done = 0; xfer.total = 0; }
            json_free(v);
          }
        }
        free(r); if (!ready) usleep(400000);
      }
      if (cancelled || quit_flag) { toast_set(T("Trasferimento annullato")); xfer.active = 0; break; }
      if (!ready) { toast_set(fail); xfer.active = 0; break; }
      {
        char dest[420], part[430], idsafe[64]; snprintf(idsafe, sizeof idsafe, "%.20s", j->id); for (char *p = idsafe; *p; p++) if (*p == '/' || *p == ':' || *p == '\\' || *p == '=') *p = '_';
        if (isvid) { sceIoMkdir("ux0:video", 0777); sceIoMkdir("ux0:video/WhatsApp", 0777); snprintf(dest, sizeof dest, "ux0:video/WhatsApp/WA_%s.mp4", idsafe); }
        else if (isaud) { sceIoMkdir("ux0:music", 0777); sceIoMkdir("ux0:music/WhatsApp", 0777); snprintf(dest, sizeof dest, "ux0:music/WhatsApp/WA_%s.mp3", idsafe); }
        else { sceIoMkdir("ux0:data/whatsapp", 0777); sceIoMkdir("ux0:data/whatsapp/downloads", 0777); const char *nm = j->text[0] ? j->text : "file"; snprintf(dest, sizeof dest, "ux0:data/whatsapp/downloads/%s", nm); for (char *p = dest + 28; *p; p++) if (*p == '/' || *p == ':' || *p == '\\') *p = '_'; }
        snprintf(part, sizeof part, "%s.part", dest);
        pthread_mutex_lock(&mu); snprintf(xfer.title, sizeof xfer.title, "%s", isvid ? T("Video") : isaud ? T("Audio") : T("File")); pthread_mutex_unlock(&mu);
        xfer_phase(T("Scarico sulla Vita")); xfer.done = 0; xfer.total = 0;
        snprintf(url, sizeof url, "%s/prep-file/%s", base, key);
        int rc = http_download_file(url, part, 900, xfer_dl_cb);
        if (rc == -2) { toast_set(T("Trasferimento annullato")); xfer.active = 0; break; }
        if (rc == 0) sceIoRemove(dest);   // se c'era una copia precedente la si sostituisce
        if (rc != 0 || sceIoRename(part, dest) < 0) { sceIoRemove(part); toast_set(T("Impossibile salvare il file")); xfer.active = 0; break; }
        toast_set(isvid ? T("Video salvato in ux0:video/WhatsApp: aprilo con l'app Video") : isaud ? T("Audio salvato in ux0:music/WhatsApp: aprilo con l'app Musica") : T("File salvato in ux0:data/whatsapp/downloads"));
        crumb("trasferimento completato: %s", dest);
      }
      xfer.active = 0;
      prep_done: break;
    }
    case J_VPREP: {
      int gen = atoi(j->extra); vp_job_gen = gen; char e[500], e2b[200], key[64] = "", cf[300], part[310];
      url_enc(j->jid, e, sizeof e); url_enc(j->id, e2b, sizeof e2b); vp_cachefile(j->id, !strcmp(j->kind, "audio"), cf, sizeof cf); snprintf(part, sizeof part, "%s.part", cf);
      snprintf(url, sizeof url, "%s/prep/%s/%s?kind=%s", base, e, e2b, !strcmp(j->kind, "audio") ? "audio" : "video");
      { char *r = NULL; size_t rl = 0; int rc = http_get_t(url, &r, &rl, 20); jv *v = (rc == 0 && r) ? json_parse(r) : NULL; free(r); if (v) { snprintf(key, sizeof key, "%s", json_str(v, "key", "")); json_free(v); } }
      if (!key[0]) { crumb("lettore: il bridge non ha risposto a /prep"); if (gen == vp.gen) vp.state = 3; break; }
      int ready = 0; unsigned long long t0 = sceKernelGetProcessTimeWide();
      while (!ready && gen == vp.gen && !quit_flag) {
        if (sceKernelGetProcessTimeWide() - t0 > 600ULL * 1000000ULL) break;
        snprintf(url, sizeof url, "%s/prep-status/%s", base, key); char *r = NULL; size_t rl = 0;
        if (http_get_t(url, &r, &rl, 10) == 0 && r) { jv *v = json_parse(r); if (v) { const char *st = json_str(v, "state", "");
          if (!strcmp(st, "ready")) ready = 1; else if (!strcmp(st, "error")) { json_free(v); free(r); if (gen == vp.gen) vp.state = 3; goto vprep_done; }
          else if (!strcmp(st, "convert")) { vp.phase = 1; vp.pct = (int)json_num(v, "pct", 0); } else { vp.phase = 0; vp.pct = 0; }
          json_free(v); } }
        free(r); if (!ready) usleep(400000);
      }
      if (!ready || gen != vp.gen) { if (gen == vp.gen) vp.state = 3; break; }
      vp.phase = 2; vp.pct = 0; sceIoMkdir("ux0:data/whatsapp", 0777); sceIoMkdir("ux0:data/whatsapp/cache", 0777);
      snprintf(url, sizeof url, "%s/prep-file/%s", base, key);
      int drc = http_download_file(url, part, 300, vp_dl_cb); if (drc != 0) { crumb("lettore: download fallito (%d) %s", drc, url); sceIoRemove(part); if (gen == vp.gen) vp.state = 3; break; }
      sceIoRemove(cf); sceIoRename(part, cf);
      if (gen == vp.gen) { snprintf(vp.path, sizeof vp.path, "%s", cf); vp.state = 2; }
      vprep_done: break;
    }
    case J_QUERY: {
      char *resp = NULL; snprintf(url, sizeof url, "%s%s", base, j->path); int r;
      if (j->text[0]) r = http_post_json_resp(url, j->text, &resp); else { size_t l; r = http_get(url, &resp, &l); }
      jv *v = (r == 0 && resp) ? json_parse(resp) : NULL; free(resp);
      int ok = v && !json_get(v, "error");
      if (j->extra[0]) toast_set(ok ? j->extra : T("Operazione non riuscita"));
      if (j->id[0]) { if (!v) v = json_parse("{\"error\":\"x\"}");
        for (int w8 = 0; w8 < 60 && !quit_flag; w8++) { pthread_mutex_lock(&mu); int busy = pend_q != NULL; pthread_mutex_unlock(&mu); if (!busy) break; usleep(5000); }   // aspetta che l'app abbia letto il risultato precedente
        pthread_mutex_lock(&mu); json_free(pend_q); pend_q = v; snprintf(pend_q_tag, sizeof pend_q_tag, "%s", j->id); pthread_mutex_unlock(&mu); }
      else json_free(v);
      break;
    }
    case J_EXPORT: {
      char *buf = NULL; size_t len = 0; url_enc(j->jid, e1, sizeof e1); snprintf(url, sizeof url, "%s/export/%s", base, e1);
      if (http_get(url, &buf, &len)) { toast_set(T("Esportazione non riuscita")); break; }
      sceIoMkdir("ux0:data/whatsapp/export", 0777);
      char fn[300]; snprintf(fn, sizeof fn, "ux0:data/whatsapp/export/%s.txt", j->text);
      for (char *p = fn + 25; *p; p++) if (*p == '/' || *p == ':' || *p == '\\') *p = '_';
      SceUID f = sceIoOpen(fn, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
      if (f >= 0) { size_t w = 0; while (w < len) { int r2 = sceIoWrite(f, buf + w, len - w); if (r2 <= 0) break; w += r2; } sceIoClose(f); toast_set(T("Chat salvata in ux0:data/whatsapp/export")); } else toast_set(T("Impossibile salvare"));
      free(buf); break;
    }
    case J_READ: {
      char *buf = NULL; size_t len; url_enc(j->jid, e1, sizeof e1);
      snprintf(url, sizeof url, "%s/read/%s", base, e1);
      if (http_get(url, &buf, &len) == 0) free(buf);
      break;
    }
  }
}

static void save_base_url(const char *u) {
  if (!strncmp(u, "engine:", 7)) return;   // motore interno: non si tocca nessun file di configurazione
  SceUID f = sceIoOpen("ux0:data/whatsapp/config.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (f >= 0) { sceIoWrite(f, u, strlen(u)); sceIoWrite(f, "\n", 1); sceIoClose(f); }
  f = sceIoOpen("ur0:tai/wa_config.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);   // copia per il plugin: sta in ur0:tai come i file degli altri plugin
  if (f >= 0) { sceIoWrite(f, u, strlen(u)); sceIoWrite(f, "\n", 1); sceIoClose(f); }
}
// ---- motore WhatsApp interno (QuickJS + Baileys) in un thread proprio ----
static Engine *g_engine; static pthread_t engine_th; static int engine_started;
static void engine_log(const char *l) { crumb("motore: %s", l); }
static void *engine_main(void *arg) {
  (void)arg; sceIoMkdir("ux0:data/whatsapp/engine", 0777);
  scePowerSetArmClockFrequency(444); scePowerSetBusClockFrequency(222); scePowerSetGpuClockFrequency(222); scePowerSetGpuXbarClockFrequency(166);   // il motore e' molto piu' veloce a 444 MHz che ai 333 predefiniti
  g_engine = eng_new("app0:assets/cacert.pem", "ux0:data/whatsapp/engine", engine_log); net_set_engine(g_engine);
  if (eng_run_module(g_engine, "app0:assets/engine.qbc") == 0) while (!quit_flag && eng_loop_step(g_engine, 20) > 0) {}
  eng_shutdown(g_engine); crumb("motore fermo (chiavi salvate)"); return NULL;
}
static void engine_start(void) {
  if (engine_started || strncmp(base_url, "engine:", 7)) return; engine_started = 1;
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 4 * 1024 * 1024); pthread_create(&engine_th, &at, engine_main, NULL); pthread_attr_destroy(&at);
}
// cerca il bridge (porta 8787) nella stessa sottorete /24 della Vita
static void scan_bridge(void) {
  if (!strncmp(base_url, "engine:", 7)) return;   // motore interno: niente da cercare in rete
  SceNetCtlInfo info; if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) return;
  int a, b, c, d; if (sscanf(info.ip_address, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return;
  for (int i = 1; i < 255 && !quit_flag; i++) {
    scan_pct = i * 100 / 254; if (i == d) continue;
    char url[96]; snprintf(url, sizeof url, "http://%d.%d.%d.%d:8787/status", a, b, c, i);
    if (http_probe(url) == 0) {
      char base[64]; snprintf(base, sizeof base, "http://%d.%d.%d.%d:8787", a, b, c, i);
      pthread_mutex_lock(&mu); snprintf(base_url, sizeof base_url, "%s", base); pthread_mutex_unlock(&mu);
      save_base_url(base); toast_set(T("Bridge trovato in rete")); break;
    }
  }
  scan_pct = -1;
}

// video/audio/file: attendono la conversione sul bridge anche per minuti, quindi hanno un thread proprio (la coda resta libera)
typedef struct { Job j; char base[256]; } LongJob;
static volatile int long_jobs;
static void *long_job_main(void *a) { LongJob *lj = a; run_job(&lj->j, lj->base); free(lj); __sync_fetch_and_sub(&long_jobs, 1); return NULL; }
static void spawn_long_job(const Job *j, const char *base) {
  if (__sync_fetch_and_add(&long_jobs, 1) >= 4) { __sync_fetch_and_sub(&long_jobs, 1); if (j->type == J_PREP) { xfer.active = 0; toast_set(T("Occupato, riprova")); } return; }
  LongJob *lj = malloc(sizeof *lj); if (!lj) { __sync_fetch_and_sub(&long_jobs, 1); return; }
  lj->j = *j; snprintf(lj->base, sizeof lj->base, "%s", base);
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 512 * 1024); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_t th; if (pthread_create(&th, &at, long_job_main, lj) != 0) { free(lj); __sync_fetch_and_sub(&long_jobs, 1); }
  pthread_attr_destroy(&at);
}
static void *worker(void *arg) {
  (void)arg;
  unsigned tick = 0, last_scan = 0;
  while (!quit_flag) {
    char url[700], *buf = NULL; size_t len = 0;
    char base[256]; pthread_mutex_lock(&mu); snprintf(base, sizeof base, "%s", base_url); pthread_mutex_unlock(&mu);

    for (;;) { // coda comandi
      Job j; pthread_mutex_lock(&mu);
      if (njobs == 0) { pthread_mutex_unlock(&mu); break; }
      j = jobs[0]; memmove(&jobs[0], &jobs[1], sizeof(Job) * (njobs - 1)); njobs--;
      pthread_mutex_unlock(&mu);
      if (j.type == J_VPREP || j.type == J_PREP) { spawn_long_job(&j, base); continue; }   // le attese lunghe (conversione) non devono bloccare la coda
      run_job(&j, base);
    }

    snprintf(url, sizeof url, "%s/status", base);
    if (http_get(url, &buf, &len) == 0) {
      jv *j = json_parse(buf); free(buf); buf = NULL;
      pthread_mutex_lock(&mu);
      net_ok = 1;
      { char ns[24]; snprintf(ns, sizeof ns, "%s", json_str(j, "status", "?")); if (strcmp(ns, wa_status)) crumb("stato bridge: %s", ns); snprintf(wa_status, sizeof wa_status, "%s", ns); }
      eng_busy_s = json_bool(j, "busy", 0) ? (int)json_num(j, "busy_s", 1) + 1 : 0; jv *sy = json_get(j, "sync"); int was = sync_active; sync_active = sy ? json_bool(sy, "active", 0) : 0; sync_pct = sy ? (int)json_num(sy, "progress", 0) : 0; sync_chats = sy ? (int)json_num(sy, "chats", 0) : 0;
      if (sync_active && !was) { sync_t0 = frame_no; crumb("sync: INIZIO"); }
      if (!sync_active && was) crumb("sync: FINE");
      { static int lastp = -1; if (sync_active && sync_pct != lastp) { crumb("sync %d%% chat=%d", sync_pct, sync_chats); lastp = sync_pct; } }
      int want_qr = !strcmp(wa_status, "qr"), is_open = !strcmp(wa_status, "open");
      pthread_mutex_unlock(&mu);
      json_free(j);
      if (want_qr && tick % 3 == 0) {
        snprintf(url, sizeof url, "%s/qr.png", base);
        if (http_get(url, &buf, &len) == 0) { pthread_mutex_lock(&mu); free(qr_png); qr_png = buf; qr_len = len; pthread_mutex_unlock(&mu); }
      }
      if (is_open) {
        char oj[160]; pthread_mutex_lock(&mu); snprintf(oj, sizeof oj, "%s", open_jid); pthread_mutex_unlock(&mu);
        if (oj[0]) {
          char e[400]; url_enc(oj, e, sizeof e);
          snprintf(url, sizeof url, "%s/messages/%s?n=%d", base, e, msg_limit);
          unsigned long long tm0 = sceKernelGetProcessTimeWide();
          int rm = http_get(url, &buf, &len);
          { static unsigned long long lastlog; if (rm != 0 || sceKernelGetProcessTimeWide() - lastlog > 8000000ULL || len > 60000) { crumb("messaggi: %s, %u byte in %u ms (n=%d)", rm == 0 ? "ok" : "ERRORE", (unsigned)len, (unsigned)((sceKernelGetProcessTimeWide() - tm0) / 1000), msg_limit); lastlog = sceKernelGetProcessTimeWide(); } }
          if (rm == 0) { jv *m = json_parse(buf); free(buf); pthread_mutex_lock(&mu); json_free(pend_msgs); pend_msgs = m; snprintf(pend_msgs_jid, sizeof pend_msgs_jid, "%s", oj); pthread_mutex_unlock(&mu); }
          snprintf(url, sizeof url, "%s/presence/%s", base, e);
          if (http_get(url, &buf, &len) == 0) { jv *m = json_parse(buf); free(buf); pthread_mutex_lock(&mu); snprintf(pres_state, sizeof pres_state, "%s", json_str(m, "state", "")); snprintf(pres_jid, sizeof pres_jid, "%s", oj); pthread_mutex_unlock(&mu); json_free(m); }
        }
        if (!oj[0] || tick % 6 == 0) {   // leggendo una chat la lista (26 KB) basta ogni tanto: i messaggi hanno la precedenza
        snprintf(url, sizeof url, "%s/chats?limit=%d", base, chat_limit);
        if (http_get(url, &buf, &len) == 0) { { static size_t lastl; if (len / 4096 != lastl / 4096) crumb("risposta /chats %u byte", (unsigned)len); lastl = len; } jv *c = json_parse(buf); free(buf); pthread_mutex_lock(&mu); json_free(pend_chats); pend_chats = c; pthread_mutex_unlock(&mu); }
        }
        if (tick % 20 == 0) {
          snprintf(url, sizeof url, "%s/profile", base);
          if (http_get(url, &buf, &len) == 0) { jv *c = json_parse(buf); free(buf); pthread_mutex_lock(&mu); json_free(pend_prof); pend_prof = c; pthread_mutex_unlock(&mu); }
        }
        // media in coda (fino a 3 per giro), poi un avatar
        for (int n = 0; n < 3; n++) {
          char mj[160] = "", mid[64] = ""; int mi = -1;
          pthread_mutex_lock(&mu);
          for (int i = 0; i < nmed; i++) if (med[i].state == 0) { snprintf(mj, sizeof mj, "%s", med[i].jid); snprintf(mid, sizeof mid, "%s", med[i].id); med[i].state = 3; mi = i; break; }
          pthread_mutex_unlock(&mu);
          if (mi < 0) break;
          char e[400], e2[200]; url_enc(mj, e, sizeof e); url_enc(mid, e2, sizeof e2);
          snprintf(url, sizeof url, "%s/media/%s/%s", base, e, e2);
          unsigned long long tm1 = sceKernelGetProcessTimeWide();
          int r = http_get(url, &buf, &len);
          crumb("media %.12s: %s %u byte in %u ms", mid, r == 0 ? "ok" : "ERRORE", (unsigned)len, (unsigned)((sceKernelGetProcessTimeWide() - tm1) / 1000));
          pthread_mutex_lock(&mu);
          for (int i = 0; i < nmed; i++) if (!strcmp(med[i].jid, mj) && !strcmp(med[i].id, mid)) {
            if (r == 0 && len > 100) { med[i].raw = (unsigned char *)buf; med[i].rawlen = len; med[i].state = 1; buf = NULL; } else med[i].state = 2;
            break;
          }
          pthread_mutex_unlock(&mu); free(buf); buf = NULL;
        }
        { // foto grande per il visualizzatore (solo quella aperta)
          char mj[160] = "", mid[64] = ""; int bi = -1;
          pthread_mutex_lock(&mu);
          for (int i = 0; i < nmed; i++) if (med[i].bstate == 4) { snprintf(mj, sizeof mj, "%s", med[i].jid); snprintf(mid, sizeof mid, "%s", med[i].id); med[i].bstate = 3; bi = i; break; }
          pthread_mutex_unlock(&mu);
          if (bi >= 0) {
            char e[400], e2[200]; url_enc(mj, e, sizeof e); url_enc(mid, e2, sizeof e2);
            snprintf(url, sizeof url, "%s/media/%s/%s?big=1", base, e, e2);
            unsigned long long tm1 = sceKernelGetProcessTimeWide(); int r = http_get(url, &buf, &len);
            crumb("foto grande %.12s: %s %u byte in %u ms", mid, r == 0 ? "ok" : "ERRORE", (unsigned)len, (unsigned)((sceKernelGetProcessTimeWide() - tm1) / 1000));
            pthread_mutex_lock(&mu);
            for (int i = 0; i < nmed; i++) if (!strcmp(med[i].jid, mj) && !strcmp(med[i].id, mid)) {
              if (r == 0 && len > 100) { med[i].braw = (unsigned char *)buf; med[i].brawlen = len; med[i].bstate = 1; buf = NULL; } else med[i].bstate = 2;
              break;
            }
            pthread_mutex_unlock(&mu); free(buf); buf = NULL;
          }
        }
        char aj[160] = "";
        pthread_mutex_lock(&mu);
        for (int i = 0; i < nav; i++) if (avs[i].state == 0) { snprintf(aj, sizeof aj, "%s", avs[i].jid); avs[i].state = 3; break; }
        pthread_mutex_unlock(&mu);
        if (aj[0]) {
          char e[400]; url_enc(aj, e, sizeof e);
          snprintf(url, sizeof url, "%s/avatar/%s", base, e);
          int r = http_get(url, &buf, &len);
          { static int avn; if (r != 0 || (++avn % 10) == 0) crumb("avatar #%d: %s %u byte", avn, r == 0 ? "ok" : "assente", (unsigned)len); }
          pthread_mutex_lock(&mu);
          Avatar *a = av_find(aj, 0);
          if (a) { if (r == 0 && len > 100) { a->raw = (unsigned char *)buf; a->rawlen = len; a->state = 1; buf = NULL; } else a->state = 2; }
          pthread_mutex_unlock(&mu); free(buf);
        }
      }
    } else { static int warned; if (!warned) { crumb("bridge non raggiungibile: %s", base); warned = 1; } pthread_mutex_lock(&mu); net_ok = 0; pthread_mutex_unlock(&mu); if (!last_scan || tick - last_scan > 40) { last_scan = tick ? tick : 1; scan_bridge(); } }
    tick++;
    for (int i = 0; i < (open_jid[0] ? 8 : 20) && !quit_flag; i++) {
      pthread_mutex_lock(&mu);
      int wake = njobs > 0 || poll_now;
      int busy = 0; for (int k = 0; k < nmed; k++) if (med[k].state == 0) busy = 1; for (int k = 0; k < nav; k++) if (avs[k].state == 0) busy = 1;
      poll_now = 0; pthread_mutex_unlock(&mu);
      if (wake || (busy && i >= 2)) break;
      usleep(100 * 1000);
    }
  }
  return NULL;
}

// ======================================================================
// emoji: atlante 32px + indice
// ======================================================================
static unsigned *emo_cp; static int emo_n;       // in ordine di atlante
static unsigned *emo_sorted_cp; static int *emo_sorted_idx;
static void emoji_init(void) {
  emo_tex = vita2d_load_PNG_file("app0:assets/emoji.png");
  size_t len; unsigned char *b = read_file("app0:assets/emoji.bin", &len);
  if (!b) return;
  emo_n = len / 4; emo_cp = malloc(len); memcpy(emo_cp, b, len); free(b);
  unsigned long long *pairs = malloc(sizeof(unsigned long long) * emo_n);
  for (int i = 0; i < emo_n; i++) pairs[i] = ((unsigned long long)emo_cp[i] << 32) | i;
  for (int i = 1; i < emo_n; i++) { unsigned long long v = pairs[i]; int k = i - 1; while (k >= 0 && pairs[k] > v) { pairs[k + 1] = pairs[k]; k--; } pairs[k + 1] = v; }
  emo_sorted_cp = malloc(sizeof(unsigned) * emo_n); emo_sorted_idx = malloc(sizeof(int) * emo_n);
  for (int i = 0; i < emo_n; i++) { emo_sorted_cp[i] = pairs[i] >> 32; emo_sorted_idx[i] = (int)(pairs[i] & 0xFFFFFFFF); }
  free(pairs);
}
static int emo_find(unsigned cp) {
  if (!emo_n || cp < 0xA9) return -1;
  int lo = 0, hi = emo_n - 1;
  while (lo <= hi) { int m = (lo + hi) / 2; if (emo_sorted_cp[m] == cp) return emo_sorted_idx[m]; if (emo_sorted_cp[m] < cp) lo = m + 1; else hi = m - 1; }
  return -1;
}
static unsigned next_cp(const char **sp) {
  const unsigned char *s = (const unsigned char *)*sp; unsigned cp; int l;
  if (s[0] < 0x80) { cp = s[0]; l = 1; } else if ((s[0] >> 5) == 6) { cp = s[0] & 31; l = 2; } else if ((s[0] >> 4) == 14) { cp = s[0] & 15; l = 3; } else if ((s[0] >> 3) == 30) { cp = s[0] & 7; l = 4; } else { *sp += 1; return '?'; }
  for (int i = 1; i < l; i++) { if ((s[i] & 0xC0) != 0x80) { *sp += i; return '?'; } cp = (cp << 6) | (s[i] & 63); }
  *sp += l; return cp;
}
// Con Roboto i simboli e gli ideogrammi (da U+2300 a U+FFFF) verrebbero disegnati come quadratini: si saltano. Con un font asiatico attivo (cjk_ok) si disegnano.
static int is_cjk_cp(unsigned cp) { return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF); }
static int cp_skip(unsigned cp) { return cp == 0xFE0F || cp == 0x200D || cp == 0x20E3 || (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0x1F1E6 && cp <= 0x1F1FF) || (cp >= 0xE0000 && cp <= 0xE007F) || (cp >= 0x2300 && cp < 0x10000 && !(cjk_ok && is_cjk_cp(cp))); }
#define ESZ(size) ((int)((size) * 1.3f))

// misura/disegna una stringa con emoji inline (sprite) e testo (Roboto)
static int etext_impl(int draw, int x, int y, unsigned col, int size, const char *s) {
  int psize = quant_size(rnd(size * UK));        // dimensione reale del font in pixel (quantizzata)
  vita2d_font *fnt = font_override ? font_override : font_for(psize);
  char run[600]; int rl = 0; float cx = (float)x;               // cx in unita' logiche
  #define FLUSH_RUN() do { if (rl) { run[rl] = 0; if (draw) vita2d_font_draw_text(fnt, rnd(cx * UK), rnd((y + size) * UK), col, psize, run); cx += fw(fnt, psize, run) / UK; rl = 0; } } while (0)
  while (*s) {
    const char *p = s; unsigned cp = next_cp(&s);
    int ei = emo_find(cp);
    if (ei < 0 && (cp_skip(cp) || cp >= 0x1F000)) continue;
    if (ei >= 0) {
      FLUSH_RUN();
      if (draw && emo_tex) { float sc = ESZ(size) / 32.0f; vita2d_draw_texture_part_scale(emo_tex, cx, y - size * 0.08f, (ei % 32) * 32 + 0.5f, (ei / 32) * 32 + 0.5f, 31, 31, sc * 32 / 31, sc * 32 / 31); }
      cx += ESZ(size) + 1;
    } else {
      int l = (int)(s - p);
      if (rl + l >= (int)sizeof run - 1) FLUSH_RUN();
      memcpy(run + rl, p, l); rl += l;
    }
  }
  FLUSH_RUN();
  return (int)ceilf(cx - x);
}
static int tw(int size, const char *s) { return etext_impl(0, 0, 0, 0, size, s); }
static void text(int x, int y, unsigned col, int size, const char *s) { etext_impl(1, x, y, col, size, s); }
static void textb(int x, int y, unsigned col, int size, const char *s) { text(x, y, col, size, s); text(x + 1, y, col, size, s); }
static void append_cp(char *buf, size_t n, unsigned cp) {
  char b[8]; int k = 0;
  if (cp < 0x80) b[k++] = cp; else if (cp < 0x800) { b[k++] = 0xC0 | (cp >> 6); b[k++] = 0x80 | (cp & 63); }
  else if (cp < 0x10000) { b[k++] = 0xE0 | (cp >> 12); b[k++] = 0x80 | ((cp >> 6) & 63); b[k++] = 0x80 | (cp & 63); }
  else { b[k++] = 0xF0 | (cp >> 18); b[k++] = 0x80 | ((cp >> 12) & 63); b[k++] = 0x80 | ((cp >> 6) & 63); b[k++] = 0x80 | (cp & 63); }
  b[k] = 0; if (strlen(buf) + k + 1 < n) strcat(buf, b);
}
static void emoji_cp(unsigned cp, int x, int y, int size) { char b[8] = ""; append_cp(b, sizeof b, cp); text(x, y, C_WHITE, size, b); }
static void fit(const char *s, int maxw, int size, char *o, size_t n) {
  { int cap = SW - 90; if (maxw > cap) maxw = cap; }          // le larghezze fisse non devono uscire dallo schermo ingrandito
  snprintf(o, n, "%s", s);
  if (tw(size, o) <= maxw) return;
  size_t l = strlen(o);
  while (l > 1) {
    l--; while (l > 0 && (o[l] & 0xC0) == 0x80) l--;
    o[l] = 0; char t[420]; snprintf(t, sizeof t, "%s...", o);
    if (tw(size, t) <= maxw) { snprintf(o, n, "%s", t); return; }
  }
}

// ======================================================================
// primitive grafiche
// ======================================================================
static void rrect(float x, float y, float w, float h, float r, unsigned c) {
  x = floorf(x + 0.5f); y = floorf(y + 0.5f); w = floorf(w + 0.5f); h = floorf(h + 0.5f); r = floorf(r + 0.5f);
  if (r * 2 > h) r = h / 2; if (r * 2 > w) r = w / 2;
  vita2d_draw_rectangle(x + r, y, w - 2 * r, h, c);
  vita2d_draw_rectangle(x, y + r, r, h - 2 * r, c);
  vita2d_draw_rectangle(x + w - r, y + r, r, h - 2 * r, c);
  vita2d_draw_fill_circle(x + r, y + r, r, c); vita2d_draw_fill_circle(x + w - r, y + r, r, c);
  vita2d_draw_fill_circle(x + r, y + h - r, r, c); vita2d_draw_fill_circle(x + w - r, y + h - r, r, c);
}
static void tline(float x1, float y1, float x2, float y2, unsigned c) {   // linea spessa ~1.6 unita' logiche
  int n = rnd(UK * 1.6f); if (n < 2) n = 2; float px1 = x1 * UK, py1 = y1 * UK, px2 = x2 * UK, py2 = y2 * UK;
  for (int i = 0; i < n; i++) { float o = i - (n - 1) / 2.0f; vita2d_draw_line(px1, py1 + o, px2, py2 + o, c); vita2d_draw_line(px1 + o, py1, px2 + o, py2, c); }
}
static void check(float x, float y, unsigned c, int dbl) {
  tline(x, y + 6, x + 4, y + 10, c); tline(x + 4, y + 10, x + 12, y, c);
  if (dbl) { tline(x + 5, y + 10, x + 6, y + 9, c); tline(x + 6, y + 10, x + 14, y, c); }
}
static void back_arrow(void) { tline(24, 30, 36, 18, C_TEXT); tline(24, 30, 36, 42, C_TEXT); tline(24, 30, 44, 30, C_TEXT); }
static void send_arrow(float cx, float cy) { tline(cx - 9, cy, cx + 8, cy, C_WHITE); tline(cx, cy - 8, cx + 9, cy, C_WHITE); tline(cx, cy + 8, cx + 9, cy, C_WHITE); }
static unsigned hash_col(const char *s) {
  unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++;
  static const unsigned pal[] = { RGBA8(0x53,0xBD,0xEB,255), RGBA8(0xE5,0x6B,0x6F,255), RGBA8(0xF5,0xB6,0x4A,255), RGBA8(0x9B,0x7F,0xE8,255), RGBA8(0x4F,0xC3,0x8D,255), RGBA8(0xE9,0x8F,0xC6,255) };
  return pal[h % 6];
}

// decodifica JPEG (anche grandi: scala con libjpeg), con maschera circolare opzionale
static vita2d_texture *decode_jpeg(const unsigned char *d, size_t n, int maxdim, int circle) {
  crumb("decode jpeg %u byte max%d circle%d", (unsigned)n, maxdim, circle);
  struct jpeg_decompress_struct ci; struct jpeg_error_mgr jerr;
  ci.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&ci); jpeg_mem_src(&ci, d, n);
  if (jpeg_read_header(&ci, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&ci); return NULL; }
  int big = ci.image_width > ci.image_height ? ci.image_width : ci.image_height;
  ci.scale_num = 1; ci.scale_denom = 1;
  while (ci.scale_denom < 8 && big / (ci.scale_denom * 2) >= maxdim) ci.scale_denom *= 2;
  ci.out_color_space = JCS_RGB; jpeg_start_decompress(&ci);
  int w = ci.output_width, h = ci.output_height;
  if (w > 4000 || h > 4000) { jpeg_abort_decompress(&ci); jpeg_destroy_decompress(&ci); return NULL; }
  vita2d_texture *t = vita2d_create_empty_texture_format(w, h, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
  if (!t) { jpeg_abort_decompress(&ci); jpeg_destroy_decompress(&ci); return NULL; }
  uint32_t *px = vita2d_texture_get_datap(t); int stride = vita2d_texture_get_stride(t) / 4;
  unsigned char *row = malloc(w * 3);
  float r = (w < h ? w : h) / 2.0f, cx = w / 2.0f, cy = h / 2.0f;
  for (int y = 0; ci.output_scanline < ci.output_height; y++) {
    unsigned char *rp = row; jpeg_read_scanlines(&ci, &rp, 1);
    for (int x = 0; x < w; x++) {
      int a = 255;
      if (circle) { float dx = x + 0.5f - cx, dy = y + 0.5f - cy; float al = r - sqrtf(dx * dx + dy * dy) + 0.5f; al = al < 0 ? 0 : al > 1 ? 1 : al; a = (int)(al * 255); }
      px[y * stride + x] = RGBA8(row[x * 3], row[x * 3 + 1], row[x * 3 + 2], a);
    }
  }
  free(row); jpeg_finish_decompress(&ci); jpeg_destroy_decompress(&ci);
  vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
  return t;
}
static vita2d_texture *decode_any(const unsigned char *d, size_t n, int maxdim) {
  vita2d_texture *t = NULL;
  if (n > 8 && d[0] == 0x89 && d[1] == 'P') t = vita2d_load_PNG_buffer(d); else t = decode_jpeg(d, n, maxdim, 0);
  if (t) vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
  return t;
}

static void draw_avatar(const char *jid, const char *name, float cx, float cy, float r) {
  Avatar *a;
  pthread_mutex_lock(&mu);
  a = av_find(jid, 1);
  if (a && a->state == 1 && a->raw) {
    unsigned char *raw = a->raw; size_t rl = a->rawlen; a->raw = NULL; a->state = 2;
    pthread_mutex_unlock(&mu);
    vita2d_texture *t = decode_jpeg(raw, rl, 512, 1); free(raw);
    pthread_mutex_lock(&mu); tex_retire(a->tex); a->tex = t;
  }
  vita2d_texture *tex = a ? a->tex : NULL;
  pthread_mutex_unlock(&mu);
  if (tex) { float s = (2 * r) / vita2d_texture_get_width(tex); vita2d_draw_texture_scale(tex, cx - r, cy - r, s, s); }
  else {
    vita2d_draw_fill_circle(cx, cy, r, hash_col(jid));
    char ini[8] = "?"; if (name[0]) { const char *q = name; next_cp(&q); int l = (int)(q - name); if (l > 6) l = 6; memcpy(ini, name, l); ini[l] = 0; if (l == 1 && ini[0] >= 'a' && ini[0] <= 'z') ini[0] -= 32; }
    int sz = (int)(r * 0.95f); int w = tw(sz, ini);
    text((int)(cx - w / 2), (int)(cy - sz * 0.62f), C_WHITE, sz, ini);
  }
}

static Media *media_get(const char *jid, const char *id) {
  char key[240]; snprintf(key, sizeof key, "%s|%s", jid, id);
  pthread_mutex_lock(&mu);
  Media *m = NULL;
  for (int i = 0; i < nmed; i++) if (!strcmp(med[i].key, key)) { m = &med[i]; break; }
  if (!m) {
    if (nmed < MAXMED) m = &med[nmed++];
    else { // sfratta il meno recente non in corso
      int best = -1; for (int i = 0; i < nmed; i++) if (med[i].state != 3 && med[i].state != 1 && (best < 0 || med[i].last < med[best].last)) best = i;
      if (best < 0) { pthread_mutex_unlock(&mu); return NULL; }
      m = &med[best]; tex_retire(m->tex); tex_retire(m->big); free(m->braw);
    }
    memset(m, 0, sizeof *m); snprintf(m->key, sizeof m->key, "%s", key); snprintf(m->jid, sizeof m->jid, "%s", jid); snprintf(m->id, sizeof m->id, "%s", id);
  }
  m->last = frame_no;
  if (m->state == 1 && m->raw) {
    unsigned char *raw = m->raw; size_t rl = m->rawlen; m->raw = NULL; m->state = 2;
    pthread_mutex_unlock(&mu);
    crumb("decodifica media %.12s (%u byte)", id, (unsigned)rl);
    vita2d_texture *t = decode_any(raw, rl, 480); free(raw);
    crumb("decodifica media -> %s", t ? "ok" : "FALLITA");
    pthread_mutex_lock(&mu); m->tex = t;
  }
  if (m->bstate == 1 && m->braw) {
    unsigned char *raw = m->braw; size_t rl = m->brawlen; m->braw = NULL; m->bstate = 2;
    pthread_mutex_unlock(&mu);
    vita2d_texture *t = decode_any(raw, rl, 1280); free(raw);
    pthread_mutex_lock(&mu); tex_retire(m->big); m->big = t;
  }
  pthread_mutex_unlock(&mu);
  return m;
}

// ======================================================================
// modelli UI
// ======================================================================
typedef struct { char jid[160]; char name[96]; char last[160]; long ts; int unread; int group; int archived, muted, blocked, channel, pinned; } Chat;
static Chat chats[400]; static int nchats;
enum { K_TEXT, K_IMAGE, K_VIDEO, K_AUDIO, K_VOICE, K_DOC, K_STICKER, K_DELETED };
typedef struct {
  char id[64]; int from_me, kind, status, edited; char *text; long ts; char name[64];
  char qtext[120], qname[48]; int hasq;
  struct { char e[12]; int n; } react[4]; int nreact;
  int mw0, mh0, dur; char fname[100]; long size;
  char **lines; int nlines; int w, h, mw, mh; unsigned laid; // layout
} Msg;
static Msg *msgs; static int nmsgs; static char msgs_jid[160];
static struct { char jid[160], name[100], about[200], phone[40]; int ok; } prof;
static long tz_off;

// +393331234567 -> "+39 333 123 4567" (prefisso italiano riconosciuto, altrimenti gruppi da 3 cifre)
static void fmt_phone(const char *digits, char *o, size_t n) {
  char d[40]; int k = 0; for (const char *p = digits; *p && k < 30; p++) if (*p >= '0' && *p <= '9') d[k++] = *p; d[k] = 0;
  if (k == 0) { snprintf(o, n, "-"); return; }
  size_t w = 0; w += snprintf(o + w, n - w, "+");
  int i = 0; if (k > 10 && d[0] == '3' && d[1] == '9') { w += snprintf(o + w, n - w, "39 "); i = 2; }
  int cnt = 0; for (; i < k && w + 3 < n; i++) { o[w++] = d[i]; cnt++; if (cnt % 3 == 0 && (k - i - 1) >= 4) o[w++] = 0x20; }
  o[w] = 0;
}
static void clean(const char *s, char *o, size_t n) { size_t k = 0; for (; *s && k + 2 < n; s++) { if ((unsigned char)*s < 0x20 && *s != '\n') continue; o[k++] = *s; } o[k] = 0; }

typedef struct { char jid[160]; Msg *m; int n; unsigned used; } MCache;
#define MCN 6
static MCache mcache[MCN];
static void msgs_release(Msg *m, int n) {
  for (int i = 0; i < n; i++) { free(m[i].text); for (int k = 0; k < m[i].nlines; k++) free(m[i].lines[k]); free(m[i].lines); }
  free(m);
}
static void free_msgs(void) { msgs_release(msgs, nmsgs); msgs = NULL; nmsgs = 0; }
// cronologia gia' scaricata: si tiene in memoria per le ultime chat, cosi' rientrando si vede subito e si aggiorna in secondo piano
static void mcache_store(void) {
  if (!msgs || !msgs_jid[0]) return;
  int slot = -1; for (int i = 0; i < MCN; i++) if (mcache[i].m && !strcmp(mcache[i].jid, msgs_jid)) slot = i;
  if (slot < 0) { for (int i = 0; i < MCN; i++) if (!mcache[i].m) { slot = i; break; } }
  if (slot < 0) { slot = 0; for (int i = 1; i < MCN; i++) if (mcache[i].used < mcache[slot].used) slot = i; }
  if (mcache[slot].m) msgs_release(mcache[slot].m, mcache[slot].n);
  snprintf(mcache[slot].jid, sizeof mcache[slot].jid, "%s", msgs_jid); mcache[slot].m = msgs; mcache[slot].n = nmsgs; mcache[slot].used = frame_no;
  msgs = NULL; nmsgs = 0; msgs_jid[0] = 0;
}
static int mcache_take(const char *jid) {
  for (int i = 0; i < MCN; i++) if (mcache[i].m && !strcmp(mcache[i].jid, jid)) { msgs = mcache[i].m; nmsgs = mcache[i].n; snprintf(msgs_jid, sizeof msgs_jid, "%s", jid); mcache[i].m = NULL; return 1; }
  return 0;
}
static int kind_of(const char *s) { return !strcmp(s, "image") ? K_IMAGE : !strcmp(s, "video") ? K_VIDEO : !strcmp(s, "audio") ? K_AUDIO : !strcmp(s, "voice") ? K_VOICE : !strcmp(s, "doc") ? K_DOC : !strcmp(s, "sticker") ? K_STICKER : !strcmp(s, "deleted") ? K_DELETED : K_TEXT; }

static int cur_chat; static Chat keep_chat; static int had_keep, keep_old;   // la chat aperta prima che l'elenco si ricostruisca (vedi remap_cur_chat)
static void apply_pending(void) {
  pthread_mutex_lock(&mu);
  if (pend_chats) {
    jv *c = pend_chats; pend_chats = NULL; pthread_mutex_unlock(&mu);
    if (c->t == J_ARR) {
      if (c->count != nchats) crumb("chat: %d", c->count);
      // La chat aperta e' un indice nell'elenco: se l'elenco si riordina (nuovo messaggio da un'altra persona) l'indice punterebbe a un'altra chat.
      // Si ricorda quale chat e' e, dopo la ricostruzione, la si ritrova per identificativo.
      if (cur_chat >= 0 && cur_chat < nchats) { keep_chat = chats[cur_chat]; had_keep = 1; keep_old = cur_chat; }
      nchats = 0;
      for (int i = 0; i < c->count && nchats < 400; i++) {
        jv *o = c->items[i]; Chat *ch = &chats[nchats++];
        snprintf(ch->jid, sizeof ch->jid, "%s", json_str(o, "jid", ""));
        clean(json_str(o, "name", ch->jid), ch->name, sizeof ch->name);
        clean(T(json_str(o, "last", "")), ch->last, sizeof ch->last);   // le anteprime generate dal bridge ("Foto", "Posizione"...) si traducono qui
        for (char *p = ch->last; *p; p++) if (*p == '\n') *p = ' ';
        ch->ts = (long)json_num(o, "ts", 0); ch->unread = (int)json_num(o, "unread", 0);
        ch->group = strstr(ch->jid, "@g.us") != NULL;
        ch->channel = strstr(ch->jid, "@newsletter") != NULL;
        ch->archived = json_bool(o, "archived", 0); ch->muted = (int)json_num(o, "muted", 0); ch->blocked = json_bool(o, "blocked", 0); ch->pinned = json_bool(o, "pinned", 0);
      }
    }
    json_free(c); pthread_mutex_lock(&mu);
  }
  if (pend_q) {
    jv *q = pend_q; pend_q = NULL; char tag[24]; snprintf(tag, sizeof tag, "%s", pend_q_tag); pthread_mutex_unlock(&mu);
    if (!strcmp(tag, qwant)) { json_free(qres); qres = q; snprintf(qtag, sizeof qtag, "%s", tag); qwait = 0; }
    else if (!strcmp(tag, qwant2)) { json_free(qres2); qres2 = q; snprintf(qtag2, sizeof qtag2, "%s", tag); }
    else json_free(q);
    pthread_mutex_lock(&mu);
  }
  if (pend_prof) {
    jv *p = pend_prof; pend_prof = NULL; pthread_mutex_unlock(&mu);
    if (json_get(p, "jid")) { snprintf(prof.jid, sizeof prof.jid, "%s", json_str(p, "jid", "")); clean(json_str(p, "name", ""), prof.name, sizeof prof.name); clean(json_str(p, "about", ""), prof.about, sizeof prof.about); fmt_phone(json_str(p, "phone", ""), prof.phone, sizeof prof.phone); prof.ok = 1; }
    json_free(p); pthread_mutex_lock(&mu);
  }
  if (pend_msgs) {
    jv *m = pend_msgs; pend_msgs = NULL; char jid[160]; snprintf(jid, sizeof jid, "%s", pend_msgs_jid); pthread_mutex_unlock(&mu);
    if (m->t == J_ARR) {
      free_msgs();
      msgs = calloc(m->count > 0 ? m->count : 1, sizeof(Msg));
      for (int i = 0; i < m->count; i++) {
        jv *o = m->items[i]; Msg *x = &msgs[nmsgs++];
        snprintf(x->id, sizeof x->id, "%s", json_str(o, "id", ""));
        char tmp[2100]; clean(T(json_str(o, "text", "")), tmp, sizeof tmp); x->text = strdup(tmp);
        x->from_me = json_bool(o, "from_me", 0); x->ts = (long)json_num(o, "ts", 0); x->kind = kind_of(json_str(o, "kind", "text"));
        x->status = (int)json_num(o, "status", 0); x->edited = json_bool(o, "edited", 0);
        clean(json_str(o, "name", ""), x->name, sizeof x->name);
        x->mw0 = (int)json_num(o, "w", 0); x->mh0 = (int)json_num(o, "h", 0); x->dur = (int)json_num(o, "dur", 0);
        clean(json_str(o, "fname", ""), x->fname, sizeof x->fname); x->size = (long)json_num(o, "size", 0);
        jv *q = json_get(o, "quoted");
        if (q && q->t == J_OBJ) { x->hasq = 1; clean(T(json_str(q, "text", "")), x->qtext, sizeof x->qtext); clean(json_str(q, "name", ""), x->qname, sizeof x->qname); }
        jv *rs = json_get(o, "reactions");
        if (rs && rs->t == J_ARR) for (int k = 0; k < rs->count && x->nreact < 4; k++) { snprintf(x->react[x->nreact].e, 12, "%s", json_str(rs->items[k], "e", "")); x->react[x->nreact].n = (int)json_num(rs->items[k], "n", 1); x->nreact++; }
      }
      msgs_gen++; if (strcmp(msgs_jid, jid)) crumb("messaggi caricati: %d", nmsgs);
      snprintf(msgs_jid, sizeof msgs_jid, "%s", jid);
    }
    json_free(m); pthread_mutex_lock(&mu);
  }
  pthread_mutex_unlock(&mu);
}

// ---- tempo ----
static void fmt_time(long ts, char *o, size_t n, int list_mode) {
  long l = ts + tz_off; time_t tt = l; struct tm *t = gmtime(&tt); struct tm a; if (t) a = *t; else memset(&a, 0, sizeof a);
  if (!list_mode) { snprintf(o, n, "%02d:%02d", a.tm_hour, a.tm_min); return; }
  SceDateTime now; sceRtcGetCurrentClockLocalTime(&now);
  SceRtcTick tk; sceRtcGetTick(&now, &tk);
  long nowsec = (long)(tk.tick / 1000000ULL) - 62135596800L;
  long days_now = nowsec / 86400, days_msg = l / 86400;
  if (days_now == days_msg) snprintf(o, n, "%02d:%02d", a.tm_hour, a.tm_min);
  else if (days_now - days_msg == 1) snprintf(o, n, T("Ieri"));
  else snprintf(o, n, "%02d/%02d/%02d", a.tm_mday, a.tm_mon + 1, a.tm_year % 100);
}
static long now_unix(void) { SceDateTime now; sceRtcGetCurrentClockLocalTime(&now); SceRtcTick tk; sceRtcGetTick(&now, &tk); return (long)(tk.tick / 1000000ULL) - 62135596800L - tz_off; }

// ======================================================================
// layout messaggi
// ======================================================================
#define BFONT 18
#define LH 24
#define PADX 10
#define PADY 6
#define HNAME 20
#define HQUOTE 48
#define HDOC 56
#define HVOICE 42
#define HTIME 16

static int has_media(const Msg *m) { return m->kind == K_IMAGE || m->kind == K_VIDEO || m->kind == K_STICKER; }
static void layout_msg(Msg *m, int maxbw, int showname) {
  if (m->laid == ui_gen) return;
  if (m->lines) { for (int k = 0; k < m->nlines; k++) free(m->lines[k]); free(m->lines); m->lines = NULL; m->nlines = 0; }
  m->laid = ui_gen;
  m->mw = m->mh = 0;
  if (has_media(m)) {
    int lim = m->kind == K_STICKER ? (int)(220.0f / UK) : (int)fminf(300.0f, 440.0f / UK); int w = m->mw0 > 0 ? m->mw0 : 4, h = m->mh0 > 0 ? m->mh0 : 3;
    float s = fminf((float)lim / w, (float)lim / h);
    m->mw = (int)(w * s); m->mh = (int)(h * s);
    if (m->mw < 120) { m->mh = m->mh * 120 / (m->mw ? m->mw : 1); m->mw = 120; }
    if (m->mh > 340) m->mh = 340;
  }
  const char *src = m->kind == K_DELETED ? T("\xF0\x9F\x9A\xAB Questo messaggio \xC3\xA8 stato eliminato") : m->text;
  int wrapw = maxbw - 2 * PADX; if (has_media(m) && m->kind != K_STICKER) wrapw = m->mw > 180 ? m->mw : 180;
  const char *s = src; char cur[2200] = ""; int cap = 8; m->lines = malloc(sizeof(char *) * cap); m->nlines = 0;
  #define PUSHLINE(str) do { if (m->nlines == cap) { cap *= 2; m->lines = realloc(m->lines, sizeof(char *) * cap); } m->lines[m->nlines++] = strdup(str); } while (0)
  if (*s) while (1) {
    char word[400]; int wl = 0;
    while (*s && *s != ' ' && *s != '\n' && wl < 380) word[wl++] = *s++;
    word[wl] = 0;
    char cand[2600]; snprintf(cand, sizeof cand, "%s%s%s", cur, cur[0] ? " " : "", word);
    if (tw(BFONT, cand) <= wrapw) snprintf(cur, sizeof cur, "%s", cand);
    else {
      if (cur[0]) { PUSHLINE(cur); cur[0] = 0; }
      char piece[400] = ""; const char *w = word;
      while (*w) {
        const char *q = w; next_cp(&q); int l = (int)(q - w);
        char t[420]; snprintf(t, sizeof t, "%s%.*s", piece, l, w);
        if (tw(BFONT, t) > wrapw && piece[0]) { PUSHLINE(piece); piece[0] = 0; continue; }
        snprintf(piece, sizeof piece, "%s", t); w += l;
      }
      snprintf(cur, sizeof cur, "%s", piece);
    }
    if (*s == '\n') { PUSHLINE(cur); cur[0] = 0; s++; if (!*s) break; continue; }
    if (*s == ' ') { s++; continue; }
    if (!*s) break;
  }
  if (*src && (cur[0] || m->nlines == 0)) PUSHLINE(cur);
  int mw = 0; for (int i = 0; i < m->nlines; i++) { int w = tw(BFONT, m->lines[i]); if (w > mw) mw = w; }
  char tm[16]; fmt_time(m->ts, tm, sizeof tm, 0);
  int tmw = tw(12, tm) + (m->from_me ? 24 : 0) + (m->edited ? tw(12, T("Modificato ")) : 0);
  int cw = mw;
  if (has_media(m) && m->mw > cw) cw = m->mw;
  if (m->hasq && cw < 200) cw = 200;
  if (m->kind == K_DOC && cw < 260) cw = 260;
  if ((m->kind == K_VOICE || m->kind == K_AUDIO) && cw < 230) cw = 230;
  int overlay = has_media(m) && m->nlines == 0;
  int extra = 0;
  if (m->kind == K_STICKER) { m->w = m->mw; m->h = m->mh + 18 + (showname ? HNAME : 0); return; }
  if (!overlay) {
    if (m->nlines > 0) { int lastw = tw(BFONT, m->lines[m->nlines - 1]); if (lastw + tmw + 14 > cw) { if (lastw + tmw + 14 <= wrapw) cw = lastw + tmw + 14; else extra = 1; } }
    else { if (cw < tmw + 14) cw = tmw + 14; extra = 1; }
  }
  int pad = has_media(m) ? 4 : PADX;
  m->w = cw + 2 * pad;
  int h = PADY + (showname ? HNAME : 0) + (m->hasq ? HQUOTE + 4 : 0);
  if (has_media(m)) h += m->mh + 4;
  if (m->kind == K_DOC) h += HDOC + 4;
  if (m->kind == K_VOICE || m->kind == K_AUDIO) h += HVOICE + 2;
  h += m->nlines * LH + (extra ? HTIME : 0) + PADY;
  m->h = h;
}

// ======================================================================
// pulsanti dichiarati durante il disegno e risolti a fine frame
// ======================================================================
enum { B_NONE, B_ROW, B_CHIP, B_PROFILE, B_BACK, B_MSG, B_EMOJI_BTN, B_SEND, B_ATTACH, B_DRAFT, B_EMO_CELL, B_EMO_BACK, B_EMO_CLOSE, B_MENU_REACT, B_MENU_ITEM, B_MENU_CLOSE, B_VIEW_CLOSE, B_PICK_ENTRY, B_PICK_UP, B_PREVIEW_SEND, B_PREVIEW_CAPTION, B_PREVIEW_CANCEL, B_PROF_NAME, B_PROF_ABOUT, B_PROF_PHOTO, B_CANCEL_REPLY, B_TAB, B_FAB, B_SEARCH_ICON, B_LISTMENU, B_CHATMENU, B_HEADER_INFO, B_POPUP_ITEM, B_POPUP_CLOSE, B_UNBLOCK_BAR, B_SEARCH_BOX, B_SEARCH_ROW, B_NC_GROUP, B_NC_CONTACT, B_NC_ROW, B_GR_SUBJECT, B_GR_ROW, B_GR_CREATE, B_CT_BLOCK, B_MEDIA_TAB, B_MEDIA_ITEM, B_DOC_ITEM, B_UPD_ROW, B_COMM_ROW, B_ST_PREV, B_ST_NEXT, B_LOGOUT, B_PLUGIN, B_PLUGIN_RM, B_BRIDGE_ADDR, B_THEME_UI, B_UPD_MY, B_STORY_NEW, B_FOLLOW_CH, B_ST_VIEWERS, B_VIEW_SAVE, B_VIEW_REPLY, B_VIEW_REACT, B_NOTIF_TEST, B_PLUGIN_DIAG , B_CAM_OPEN, B_CAM_SHOT, B_CAM_FLIP, B_COMM_NEW, B_COMM_HEAD, B_UPD_CHECK, B_UPD_CANCEL, B_UPD_BLOCK, B_LANG, B_LANG_ROW, B_XFER_CANCEL, B_ST_SAVE, B_CALL_ROW, B_ABOUT, B_SEL_CANCEL, B_SEL_PIN, B_SEL_MUTE, B_SEL_ARCH, B_SEL_DEL};
typedef struct { int x, y, w, h, id, arg; } Btn;
static Btn btns[400]; static int nb; static int clip_y0 = 0, clip_y1 = 100000;
static void btn(int x, int y, int w, int h, int id, int arg) {
  if (nb >= 400) return;
  int y1 = y + h; if (y < clip_y0) y = clip_y0; if (y1 > clip_y1) y1 = clip_y1; if (y1 <= y) return;
  btns[nb++] = (Btn){ rnd(x * UK), rnd(y * UK), rnd(w * UK), rnd((y1 - y) * UK), id, arg };   // memorizzato in pixel reali: il tocco e' in pixel
}
static void btn_reset(void) { nb = 0; clip_y0 = 0; clip_y1 = SH; }
static Btn *btn_at(float x, float y) {
  for (int i = nb - 1; i >= 0; i--) if (x >= btns[i].x && x < btns[i].x + btns[i].w && y >= btns[i].y && y < btns[i].y + btns[i].h) return &btns[i];
  return NULL;
}

// ======================================================================
// IME
// ======================================================================
enum { IME_DRAFT = 1, IME_CAPTION, IME_NAME, IME_ABOUT, IME_SEARCH, IME_NUMBER, IME_CNAME, IME_GSUBJECT, IME_BRIDGE, IME_STORYTEXT, IME_CHLINK, IME_COMM_NAME, IME_COMM_DESC, IME_COMM_GNAME, IME_COMM_RENAME, IME_COMM_DESC2 };
static SceWChar16 ime_in[2049], ime_title[40];
static int ime_active, ime_ctx;
static void u8to16(const char *s, SceWChar16 *o, int n) {
  int k = 0;
  while (*s && k < n - 2) { unsigned cp = next_cp(&s); if (cp >= 0x10000) { cp -= 0x10000; o[k++] = 0xD800 + (cp >> 10); o[k++] = 0xDC00 + (cp & 0x3FF); } else o[k++] = cp; }
  o[k] = 0;
}
static void u16to8(const SceWChar16 *s, char *o, size_t n) {
  size_t k = 0;
  while (*s && k + 5 < n) {
    unsigned cp = *s++;
    if (cp >= 0xD800 && cp < 0xDC00 && *s >= 0xDC00) cp = 0x10000 + ((cp - 0xD800) << 10) + (*s++ - 0xDC00);
    if (cp < 0x80) o[k++] = cp;
    else if (cp < 0x800) { o[k++] = 0xC0 | (cp >> 6); o[k++] = 0x80 | (cp & 63); }
    else if (cp < 0x10000) { o[k++] = 0xE0 | (cp >> 12); o[k++] = 0x80 | ((cp >> 6) & 63); o[k++] = 0x80 | (cp & 63); }
    else { o[k++] = 0xF0 | (cp >> 18); o[k++] = 0x80 | ((cp >> 12) & 63); o[k++] = 0x80 | ((cp >> 6) & 63); o[k++] = 0x80 | (cp & 63); }
  }
  o[k] = 0;
}
static SceWChar16 ime_init[2049];
static int ime_try(int ctx, int variant) {
  SceImeDialogParam p; sceImeDialogParamInit(&p);
  p.supportedLanguages = 0x0001FFFF; p.languagesForced = 0; p.type = SCE_IME_TYPE_DEFAULT;
  p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
  p.title = ime_title; p.maxTextLength = 1000; p.initialText = ime_init; p.inputTextBuffer = ime_in;   // buffer iniziale e di risposta separati
  int multi = 0;   // una sola riga: cosi' il tasto Invio conferma (in modalita' multiriga non c'era e la bozza si perdeva)
  if (variant == 0) { p.option = multi ? SCE_IME_OPTION_MULTILINE : 0; p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT; }
  else if (variant == 1) { p.option = 0; p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR; }
  else { p.option = 0; p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT; p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_DEFAULT; p.maxTextLength = 200; }
  return sceImeDialogInit(&p);
}
static void ime_open(int ctx, const char *title, const char *initial) {
  if (ime_active) return;
  u8to16(title, ime_title, 40); u8to16(initial, ime_init, 2049); ime_in[0] = 0;
  int r = -1;
  for (int v = 0; v < 3 && r < 0; v++) { r = ime_try(ctx, v); crumb("tastiera: tentativo %d ctx=%d -> 0x%x", v, ctx, (unsigned)r); }
  if (r >= 0) { ime_active = 1; ime_ctx = ctx; }
  else { char m[80]; snprintf(m, sizeof m, T("Tastiera non disponibile (errore 0x%x)"), (unsigned)r); toast_set(m); }
}

// ======================================================================
// stato UI
// ======================================================================
enum { S_LOGIN, S_LIST, S_CHAT, S_PROFILE, S_PICKER, S_PREVIEW, S_UPDATES, S_COMM, S_CALLS, S_SEARCH, S_NEWCHAT, S_GROUP, S_CONTACT, S_MEDIA, S_STATUSVIEW, S_CAMERA, S_LANG, S_ABOUT };
// scala usata da ciascuna schermata: quelle con disposizione fissa (accesso, profilo, selettore file) restano piu' piccole
static float screen_scale(int scr) {
  float k = ui_k;
  switch (scr) { case S_LOGIN: case S_PROFILE: case S_PICKER: case S_PREVIEW: if (k > 1.0f) k = 1.0f; break; default: break; }
  return k;
}
static int screen = S_LOGIN;
static float list_scroll, list_vel, chat_scroll, chat_vel, emo_scroll, emo_vel, pick_scroll, pick_vel;
static int chat_stick = 1, filter_sel, cur_chat = -1;
// Dopo che l'elenco delle chat e' stato ricostruito (nell'ordine dell'ultima attivita') ritrova la chat aperta per identificativo:
// con l'indice vecchio si sarebbe finiti nella conversazione di un'altra persona (e i messaggi sarebbero partiti a lei).
static void remap_cur_chat(void) {
  if (!had_keep) return; had_keep = 0;
  int ni = -1; for (int i = 0; i < nchats; i++) if (!strcmp(chats[i].jid, keep_chat.jid)) { ni = i; break; }
  if (ni < 0 && (screen == S_CHAT || screen == S_MEDIA || screen == S_CONTACT || screen == S_SEARCH) && nchats < 400) { chats[nchats] = keep_chat; ni = nchats++; crumb("chat aperta fuori dall'elenco: la tengo (%s)", keep_chat.jid); }   // scesa oltre il limite caricato
  if (ni != keep_old) crumb("chat aperta: indice %d -> %d (elenco riordinato, %s)", keep_old, ni, keep_chat.jid);
  cur_chat = ni;
  if (ni < 0 && screen == S_CHAT) screen = S_LIST;
}

static vita2d_texture *qr_tex;
static char draft[2100], reply_id[64], reply_text[120], reply_name[48], edit_id[64];
static int emoji_open;
static char menu_id[64]; static int menu_open;
static char view_key_id[64]; static int viewer_open;
static float vz = 1.0f, vpx = 0, vpy = 0;                  // zoom e spostamento (px logici) del visualizzatore
static int vpinch, vtouch, vmoved; static float vd0, vz0, vcx0, vcy0, vpx0, vpy0, vsx, vsy, vlx, vly;
static char toast_show[160]; static int toast_frames;

typedef struct { char name[120]; int dir; } Entry;
static Entry ents[600]; static int nents; static char pick_dir[300] = "";
static int pick_mode;       // 0 allegato chat, 1 foto profilo
static char sel_path[300], sel_kind[12], caption[1000];
static vita2d_texture *prev_tex;

static void load_config(void) {
  // L'app usa sempre il motore WhatsApp interno. Solo per sviluppo: se esiste use_bridge.txt con un indirizzo http, si usa quel bridge. config.txt non si legge piu'.
  SceUID f = sceIoOpen("ux0:data/whatsapp/use_bridge.txt", SCE_O_RDONLY, 0);
  if (f < 0) return;
  char b[256] = ""; int n = sceIoRead(f, b, sizeof b - 1); sceIoClose(f);
  if (n <= 0) return; b[n] = 0;
  for (char *p = b; *p; p++) if (*p == '\n' || *p == '\r' || *p == ' ') { *p = 0; break; }
  if (!strncmp(b, "http", 4)) { while (strlen(b) > 1 && b[strlen(b) - 1] == '/') b[strlen(b) - 1] = 0; snprintf(base_url, sizeof base_url, "%s", b); }
}
static int ext_is(const char *n, const char *list) { const char *d = strrchr(n, '.'); if (!d) return 0; char e[12]; int i = 0; for (d++; *d && i < 10; d++) e[i++] = (*d >= 'A' && *d <= 'Z') ? *d + 32 : *d; e[i] = 0; char t[100]; snprintf(t, sizeof t, ",%s,", e); char l[100]; snprintf(l, sizeof l, ",%s,", list); return strstr(l, t) != NULL; }
static const char *kind_for(const char *name) { return ext_is(name, "jpg,jpeg,png,gif,bmp,webp") ? "image" : ext_is(name, "mp4,mov,m4v,3gp") ? "video" : ext_is(name, "mp3,ogg,m4a,aac,wav,opus") ? "audio" : "doc"; }

static int ent_cmp(const void *a, const void *b) { const Entry *x = a, *y = b; if (x->dir != y->dir) return y->dir - x->dir; return strcasecmp(x->name, y->name); }
static void pick_load(const char *dir) {
  nents = 0; pick_scroll = 0; pick_vel = 0;
  if (!dir[0]) {
    const char *roots[] = { "ux0:picture", "ux0:photo", "ux0:data", "ux0:", "uma0:", "ur0:" };
    for (int i = 0; i < 6; i++) { snprintf(ents[nents].name, sizeof ents[0].name, "%s", roots[i]); ents[nents++].dir = 1; }
    pick_dir[0] = 0; return;
  }
  snprintf(pick_dir, sizeof pick_dir, "%s", dir);
  SceUID d = sceIoDopen(dir); if (d < 0) return;
  SceIoDirent e;
  while (nents < 600 && sceIoDread(d, &e) > 0) {
    if (e.d_name[0] == '.') continue;
    int isd = SCE_S_ISDIR(e.d_stat.st_mode) ? 1 : 0;
    if (pick_mode >= 1 && !isd && !ext_is(e.d_name, "jpg,jpeg,png")) continue;
    snprintf(ents[nents].name, sizeof ents[0].name, "%s", e.d_name); ents[nents++].dir = isd;
  }
  sceIoDclose(d);
  qsort(ents, nents, sizeof(Entry), ent_cmp);
}
static void pick_up(void) {
  if (!pick_dir[0]) return;
  char t[300]; snprintf(t, sizeof t, "%s", pick_dir);
  size_t l = strlen(t); if (l && t[l - 1] == '/') t[--l] = 0;
  char *s = strrchr(t, '/');
  if (s) { *s = 0; if (!t[0] || t[strlen(t) - 1] == ':') { strcat(t, "/"); } pick_load(t[strlen(t) - 1] == ':' ? "" : t); }
  else pick_load("");
}
static void pick_join(char *out, size_t n, const char *name) {
  if (!pick_dir[0]) snprintf(out, n, "%s", name);
  else snprintf(out, n, "%s%s%s", pick_dir, pick_dir[strlen(pick_dir) - 1] == '/' ? "" : "/", name);
}

static Msg *find_msg(const char *id) { for (int i = 0; i < nmsgs; i++) if (!strcmp(msgs[i].id, id)) return &msgs[i]; return NULL; }

static void send_draft(void) {
  if (!draft[0] || cur_chat < 0) return;
  Job *j = job_new(J_SEND, chats[cur_chat].jid); if (!j) { toast_set(T("Coda piena, riprova")); return; }
  snprintf(j->text, sizeof j->text, "%s", draft); snprintf(j->id, sizeof j->id, "%s", reply_id); snprintf(j->extra, sizeof j->extra, "%s", edit_id);
  job_commit();
  draft[0] = 0; reply_id[0] = 0; edit_id[0] = 0; chat_stick = 1; emoji_open = 0;
}
static void open_chat(int idx) {
  crumb("APRO CHAT idx=%d nome=%.30s gruppo=%d", idx, chats[idx].name, chats[idx].group);
  msg_limit = 30; older_pending = 0; no_more_older = 0; anchor_id[0] = 0; chat_logged_frames = 0;
  cur_chat = idx; screen = S_CHAT; chat_stick = 1; chat_scroll = 0; chat_vel = 0; draft[0] = 0; reply_id[0] = 0; edit_id[0] = 0; emoji_open = 0; menu_open = 0; viewer_open = 0;
  mcache_store(); if (mcache_take(chats[idx].jid)) crumb("chat dalla memoria: %d messaggi gia' pronti", nmsgs); else { free_msgs(); msgs_jid[0] = 0; }
  pthread_mutex_lock(&mu); snprintf(open_jid, sizeof open_jid, "%s", chats[idx].jid); pres_state[0] = 0; pthread_mutex_unlock(&mu);
  Job *j = job_new(J_READ, chats[idx].jid); if (j) job_commit();
  chats[idx].unread = 0;
}
static void close_chat(void) {
  screen = S_LIST; pthread_mutex_lock(&mu); open_jid[0] = 0; pthread_mutex_unlock(&mu); mcache_store(); emoji_open = 0; menu_open = 0; viewer_open = 0;
}

#define ROWH 72
static int visible_idx[400]; static int nvis; static int show_archived;
// selezione di piu' chat nella lista (pressione lunga): in alto compaiono le azioni (fissa, silenzia, archivia, elimina)
#define MAXSEL 8
static char selj[MAXSEL][160]; static int nsel;
static int sel_find(const char *jid) { for (int i = 0; i < nsel; i++) if (!strcmp(selj[i], jid)) return i; return -1; }
static void sel_toggle(const char *jid) { int i = sel_find(jid); if (i >= 0) { memmove(selj[i], selj[i + 1], (size_t)(nsel - i - 1) * sizeof selj[0]); nsel--; } else if (nsel < MAXSEL) snprintf(selj[nsel++], sizeof selj[0], "%s", jid); }
static Chat *sel_chat(int i);
static void build_visible(void) {
  nvis = 0;
  for (int i = 0; i < nchats; i++) {
    if (chats[i].channel) continue;                 // i canali stanno nella scheda Aggiornamenti
    if (chats[i].archived != show_archived) continue;
    if (filter_sel == 1 && chats[i].unread <= 0) continue;
    if (filter_sel == 2 && !chats[i].group) continue;
    visible_idx[nvis++] = i;
  }
}
static const unsigned QUICK[6] = { 0x1F44D, 0x2764, 0x1F602, 0x1F62E, 0x1F622, 0x1F64F };
static void backspace_draft(void) { size_t l = strlen(draft); if (!l) return; l--; while (l > 0 && (draft[l] & 0xC0) == 0x80) l--; draft[l] = 0; }

#include "extra.inc"

// ======================================================================
// main
// ======================================================================
int main(void) {
  { char d = '2'; SceUID sf = sceIoOpen("ux0:data/whatsapp/ui_scale.txt", SCE_O_RDONLY, 0); if (sf >= 0) { sceIoRead(sf, &d, 1); sceIoClose(sf); } ui_k = d == '0' ? 1.0f : d == '1' ? 1.5f : 2.0f; }   // predefinito: molto grande (2x)
  { char d = '0'; SceUID tf = sceIoOpen("ux0:data/whatsapp/theme_ui.txt", SCE_O_RDONLY, 0); if (tf >= 0) { sceIoRead(tf, &d, 1); sceIoClose(tf); } apply_ui_theme(d == '1'); }   // predefinito: chiaro
  i18n_init();   // lingua scelta (o quella di sistema della Vita)
  upd_note("app: avvio (main)");   // primissima riga: se dopo un aggiornamento manca, l'app non e' nemmeno partita
  upd_init(); vp_cache_wipe(); upd_note("app: versione letta, avvio la grafica");
  vita2d_init_advanced(8 * 1024 * 1024); vita2d_set_clear_color(C_BG);   // memoria temporanea per frame: 8 MB (di default 1 MB: con l'interfaccia ingrandita poteva finire)
  pool_total = vita2d_pool_free_space();
  load_fonts();
  crumb("vita2d e font ok");
  emoji_init();
  crumb("emoji ok");
  {   // precarica i caratteri comuni nelle quattro cache, cosi' scorrendo non se ne aggiungono di nuovi (era la causa dello sfarfallio)
    static const char *cs = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]_abcdefghijklmnopqrstuvwxyz\xC3\xA0\xC3\xA8\xC3\xA9\xC3\xAC\xC3\xB2\xC3\xB9\xC3\x80\xC3\x88\xC3\x89\xC3\x8C\xC3\x92\xC3\x99\xC3\xA7\xC3\xB1\xC3\xA4\xC3\xB6\xC3\xBC\xC3\xAA\xC3\xA2\xC3\xAE\xC3\xB4\xC3\xBB\xC2\xB0\xE2\x80\x99\xE2\x80\xA6\xE2\x80\x93\xC2\xAB\xC2\xBB";
    static const char *letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static const int warm[8][2] = { {0,18}, {0,22}, {1,26}, {1,30}, {2,34}, {2,40}, {3,46}, {3,54} };
    for (int i = 0; i < 8; i++) { vita2d_start_drawing(); vita2d_clear_screen(); vita2d_font_draw_text(fonts[warm[i][0]], 0, 100, RGBA8(0, 0, 0, 0), warm[i][1], warm[i][0] == 3 ? letters : cs); vita2d_end_drawing(); vita2d_common_dialog_update(); vita2d_swap_buffers(); }
    crumb("caratteri precaricati");
  }
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
  sceIoMkdir("ux0:data/whatsapp", 0777);
  {
    SceUID lk = sceIoOpen(LOCKFILE, SCE_O_RDONLY, 0);
    if (lk >= 0) { sceIoClose(lk); crash_pending = 1; }
    size_t ll; unsigned char *lb = read_file(LOGFILE, &ll);
    if (lb) {
      if (crash_pending) { size_t st0 = ll > 900 ? ll - 900 : 0; char *tail = (char *)lb + st0; char *lines[40]; int nl = 0; char *sv; for (char *tk = strtok_r(tail, "\n", &sv); tk && nl < 40; tk = strtok_r(NULL, "\n", &sv)) lines[nl++] = tk;
        for (int i = nl > 6 ? nl - 6 : 0; i < nl; i++) { snprintf(crash_lines[crash_n++], 64, "%s", lines[i]); } }
      if (ll > 60000) sceIoRemove(LOGFILE);
      free(lb);
    }
    crumb(crash_pending ? "=== avvio (l'esecuzione precedente non e' terminata bene) ===" : "=== avvio ===");
    SceUID nl2 = sceIoOpen(LOCKFILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (nl2 >= 0) { sceIoWrite(nl2, "1", 1); sceIoClose(nl2); }
  }
  load_config();
  engine_start();
  {
    SceDateTime lt, ut; SceRtcTick a, b;
    sceRtcGetCurrentClockLocalTime(&lt); sceRtcGetCurrentClock(&ut, 0);
    sceRtcGetTick(&lt, &a); sceRtcGetTick(&ut, &b); tz_off = (long)((long long)(a.tick - b.tick) / 1000000);
  }
  extra_init();
  net_start();
  crumb("init ok, avvio worker");
  pthread_t th; pthread_attr_t pa; pthread_attr_init(&pa); pthread_attr_setstacksize(&pa, 1024 * 1024); pthread_create(&th, &pa, worker, NULL);
  crumb("worker avviato");

  SceCtrlData pad, oldpad = { 0 };
  int touching = 0, moved = 0, held = 0, lp_fired = 0; float tx0 = 0, ty0 = 0, tlast = 0;
  float *drag_sc = NULL, *drag_vel = NULL;
  int running = 1;

  while (running && !quit_req) {
    frame_no++;
    UKB = UK = screen_scale(screen);
    #define FC(m) do { if (frame_no <= 2) crumb("frame %u: %s", frame_no, m); } while (0)
    FC("inizio");
    if (frame_no % 240 == 7) { static char tpl_last[260]; char tpl[260]; snprintf(tpl, sizeof tpl, "%s\n%s\n", T("Nuovo messaggio da %s"), T("Nuovo messaggio da %s in %s")); if (strcmp(tpl, tpl_last)) { SceUID tf = sceIoOpen("ur0:tai/wa_tpl.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (tf >= 0) { sceIoWrite(tf, tpl, strlen(tpl)); sceIoClose(tf); snprintf(tpl_last, sizeof tpl_last, "%s", tpl); } } }   // testi delle notifiche nella lingua dell'app, letti dal plugin
    if (frame_no % 240 == 1) { SceUID hf = sceIoOpen("ur0:tai/wa_active.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (hf >= 0) { sceIoWrite(hf, "1", 1); sceIoClose(hf); } }
    if (strcmp(font_wanted(), font_path_loaded)) { load_fonts(); ui_gen++; crumb("lingua: font cambiato (%s)", font_path_loaded); }
    apply_pending(); remap_cur_chat(); FC("apply_pending ok");
    extra_tick(); FC("extra_tick ok");
    pthread_mutex_lock(&mu);
    if (qr_png && qr_len) { vita2d_texture *t = vita2d_load_PNG_buffer(qr_png); free(qr_png); qr_png = NULL; qr_len = 0; if (t) { if (qr_tex) { vita2d_wait_rendering_done(); vita2d_free_texture(qr_tex); } qr_tex = t; } }
    char st[24]; snprintf(st, sizeof st, "%s", wa_status); int nok = net_ok;
    if (toast_new) { toast_new = 0; snprintf(toast_show, sizeof toast_show, "%s", toast_txt); toast_frames = 180; }
    char pst[24]; snprintf(pst, sizeof pst, "%s", pres_state);
    pthread_mutex_unlock(&mu);
    if (!strcmp(st, "open") && screen == S_LOGIN) screen = S_LIST;
    if (strcmp(st, "open") && screen != S_LOGIN) screen = S_LOGIN;

    // ---- input ----
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned pressed = pad.buttons & ~oldpad.buttons; oldpad = pad;
    if (upd_busy()) pressed = 0;
    if (nsel > 0 && screen == S_LIST && !popup_open && (pressed & SCE_CTRL_CIRCLE)) { nsel = 0; pressed &= ~SCE_CTRL_CIRCLE; }   // Cerchio annulla la selezione
    if (screen != S_LIST) nsel = 0;
    if (xfer.active) { if (pressed & SCE_CTRL_CIRCLE) xfer.cancel = 1; pressed = 0; }   // durante un trasferimento: Cerchio annulla, il resto e' bloccato   // l'aggiornamento e' obbligatorio e non si interrompe: durante l'installazione non si tocca altro
    SceTouchData td; sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
    int tap = 0, lp = 0; float tapx = 0, tapy = 0;
    if (!ime_active) {
      if (viewer_open || (screen == S_STATUSVIEW && st_img_view && !popup_open)) {   // visualizzatore: pizzico con due dita = zoom, un dito (con zoom) = sposta, tocco = chiudi / torna a zoom 1
        touching = 0;
        if (td.reportNum >= 2) {
          float x0 = td.report[0].x / 2.0f, y0 = td.report[0].y / 2.0f, x1 = td.report[1].x / 2.0f, y1 = td.report[1].y / 2.0f;
          float d = hypotf(x1 - x0, y1 - y0), cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
          if (!vpinch) { vpinch = 1; vd0 = d > 1 ? d : 1; vz0 = vz; vcx0 = cx; vcy0 = cy; vpx0 = vpx; vpy0 = vpy; }
          else { vz = vz0 * d / vd0; if (vz < 0.4f) vz = 0.4f; if (vz > 6) vz = 6; vpx = vpx0 + (cx - vcx0) / UK; vpy = vpy0 + (cy - vcy0) / UK; }
          vmoved = 1; vtouch = 1;
        } else if (td.reportNum == 1) {
          float x = td.report[0].x / 2.0f, y = td.report[0].y / 2.0f;
          if (!vtouch) { vtouch = 1; vmoved = 0; vsx = x; vsy = y; vlx = x; vly = y; }
          else { if (fabsf(x - vsx) > 10 || fabsf(y - vsy) > 10) vmoved = 1; if (fabsf(vz - 1) > 0.01f && !vpinch && vmoved) { vpx += (x - vlx) / UK; vpy += (y - vly) / UK; } vlx = x; vly = y; }
        } else if (vtouch) {
          vtouch = 0; if (!vmoved && !vpinch) { tap = 1; tapx = vsx; tapy = vsy; } vpinch = 0; vmoved = 0;
        }
      } else if (td.reportNum > 0) {
        float x = (int)(td.report[0].x / 2), y = (int)(td.report[0].y / 2);
        if (!touching) {
          nav_mode = 0; touching = 1; moved = 0; held = 0; lp_fired = 0; tx0 = x; ty0 = y; tlast = y; drag_sc = NULL; drag_vel = NULL;
          if (screen == S_LIST) { drag_sc = &list_scroll; drag_vel = &list_vel; }
          else if (screen == S_CHAT) { if (emoji_open && y / UK >= SH - BARH - EMO_PANEL + 40 && y / UK < SH - BARH) { drag_sc = &emo_scroll; drag_vel = &emo_vel; } else { drag_sc = &chat_scroll; drag_vel = &chat_vel; } }
          else if (screen == S_PICKER) { drag_sc = &pick_scroll; drag_vel = &pick_vel; }
          else if (screen > S_PREVIEW) { drag_sc = &misc_scroll; drag_vel = &misc_vel; }
        } else {
          float dy = y - tlast; tlast = y;
          if (fabsf(y - ty0) > 10 || fabsf(x - tx0) > 10) moved = 1;
          if (moved && drag_sc) { *drag_sc -= dy / UK; *drag_vel = -dy / UK; if (drag_sc == &chat_scroll) chat_stick = 0; }
          if (!moved && !lp_fired && ++held == 28) { lp_fired = 1; lp = 1; tapx = tx0; tapy = ty0; }
        }
      } else if (touching) { touching = 0; if (!moved && !lp_fired) { tap = 1; tapx = tx0; tapy = ty0; } }
      if (!touching) {
        list_scroll += list_vel; list_vel *= 0.93f; chat_scroll += chat_vel; chat_vel *= 0.93f; emo_scroll += emo_vel; emo_vel *= 0.93f; pick_scroll += pick_vel; pick_vel *= 0.93f; misc_scroll += misc_vel; misc_vel *= 0.93f; if (fabsf(misc_vel) < 0.2f) misc_vel = 0;
        if (fabsf(list_vel) < 0.2f) list_vel = 0; if (fabsf(chat_vel) < 0.2f) chat_vel = 0; if (fabsf(emo_vel) < 0.2f) emo_vel = 0; if (fabsf(pick_vel) < 0.2f) pick_vel = 0;
      }
      float ay = (pad.ly - 128) / 128.0f; float stick = fabsf(ay) > 0.2f ? ay * 14 : 0;
      if (screen == S_LIST) {
        int dir = 0; if (pressed & SCE_CTRL_UP) dir = -1; if (pressed & SCE_CTRL_DOWN) dir = 1;
        if (pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN)) { dpad_hold++; if (dpad_hold > 16 && dpad_hold % 4 == 0) dir = (pad.buttons & SCE_CTRL_UP) ? -1 : 1; } else dpad_hold = 0;   // tenendo premuto si ripete
        if (dir && !popup_open && !menu_open) { if (!nav_mode) nav_mode = 1; else list_sel += dir; list_sel_dirty = 1; }
      } else if (screen == S_UPDATES || screen == S_COMM || screen == S_CALLS) {   // come la lista chat: croce = seleziona, X = apri
        int dir = 0; if (pressed & SCE_CTRL_UP) dir = -1; if (pressed & SCE_CTRL_DOWN) dir = 1;
        if (pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN)) { dpad_hold++; if (dpad_hold > 16 && dpad_hold % 4 == 0) dir = (pad.buttons & SCE_CTRL_UP) ? -1 : 1; } else dpad_hold = 0;
        if (dir && !popup_open && !menu_open && !viewer_open) { if (!tnav) tnav = 1; else tsel += dir; tsel_dirty = 1; if (tsel >= nsr) tsel = nsr - 1; if (tsel < 0) tsel = 0; }
      } else { if (pad.buttons & SCE_CTRL_UP) stick -= 10; if (pad.buttons & SCE_CTRL_DOWN) stick += 10; }
      if (stick != 0) { if (screen == S_LIST) list_scroll += stick; else if (screen == S_CHAT) { if (emoji_open) emo_scroll += stick; else { chat_scroll += stick; chat_stick = 0; } } else if (screen == S_PICKER) pick_scroll += stick; else if (screen > S_PREVIEW) misc_scroll += stick; }
    }

    {   // controllo esterno: il motore e' fermo? in quale fase? (si scrive nel registro ogni 10 s)
      static unsigned lastb; static unsigned long long tb, tl; unsigned long long nw = sceKernelGetProcessTimeWide();
      if (g_engine) { unsigned b = eng_beat(); if (b != lastb || !tb) { lastb = b; tb = nw; } else if (nw - tb > 10000000ULL && nw - tl > 10000000ULL) { tl = nw; crumb("MOTORE FERMO da %llu s nella fase: %s", (nw - tb) / 1000000ULL, eng_stage()); } }
    }
    // ---- disegno ----
    list_scroll = roundf(list_scroll); chat_scroll = roundf(chat_scroll); emo_scroll = roundf(emo_scroll); pick_scroll = roundf(pick_scroll); misc_scroll = roundf(misc_scroll);
    FC("input ok");
    btn_reset();
    tex_reap();
    vita2d_start_drawing(); vita2d_clear_screen(); FC("start_drawing ok");

    if (screen == S_LOGIN) {
      textb(60, 70, C_TEXT, 34, T("Usa WhatsApp su PS Vita"));
      const char *steps[] = { T("1. Apri WhatsApp sul telefono"), T("2. Tocca Menu o Impostazioni > Dispositivi collegati"), T("3. Tocca Collega un dispositivo"), T("4. Inquadra il codice QR a destra") };
      for (int i = 0; i < 4; i++) text(60, 140 + i * 40, C_SUB, 21, steps[i]);
      if (!nok && !strncmp(base_url, "engine:", 7)) {
        text(60, 360, C_GREEN2, 22, T("Avvio del motore WhatsApp...")); text(60, 396, C_SUB, 15, T("Puo' volerci qualche secondo."));
      } else if (!nok) {
        text(60, 360, C_RED, 19, T("Bridge non raggiungibile:")); text(60, 388, C_SUB, 19, base_url);
        if (scan_pct >= 0) { char sc[80]; snprintf(sc, sizeof sc, T("Cerco il bridge in rete... %d%%"), scan_pct); text(60, 420, C_GREEN2, 17, sc); }
        else text(60, 420, C_SUB, 15, T("Avvia il bridge sul PC (porta 8787) e tieni Vita e PC sulla stessa rete."));
        rrect(60, 456, 330, 46, 23, C_BAR); text(60 + (330 - tw(18, T("Imposta indirizzo bridge"))) / 2, 467, C_GREEN2, 18, T("Imposta indirizzo bridge")); btn(60, 456, 330, 46, B_BRIDGE_ADDR, 0);
      }
      else if (!strcmp(st, "qr") && qr_tex) {
        rrect(553, 63, 354, 354, 18, C_DIV); rrect(560, 70, 340, 340, 14, C_WHITE);
        float s = 316.0f / vita2d_texture_get_width(qr_tex); vita2d_draw_texture_scale(qr_tex, 572, 82, s, s);
      } else text(560, 220, C_SUB, 22, !strcmp(st, "loggedout") ? T("Sessione terminata, riavvio...") : T("Connessione a WhatsApp..."));
    }

    if (screen == S_LIST) {
      build_visible();
      float viewh = SH - HDR - CHIPS - TABH; float total = nvis * ROWH; float maxs = total > viewh ? total - viewh : 0;
      if (list_scroll < 0) { list_scroll = 0; list_vel = 0; } if (list_scroll > maxs) { list_scroll = maxs; list_vel = 0; }
      if (list_sel >= nvis) list_sel = nvis - 1; if (list_sel < 0) list_sel = 0;
      if (list_sel_dirty) { list_sel_dirty = 0; list_vel = 0; float top = list_sel * (float)ROWH; if (top < list_scroll) list_scroll = top; else if (top + ROWH > list_scroll + viewh) list_scroll = top + ROWH - viewh; if (list_scroll < 0) list_scroll = 0; if (list_scroll > maxs) list_scroll = maxs; }
      if (list_scroll > maxs - 250 && nchats >= chat_limit && chat_limit < 400) { chat_limit += 40; crumb("carico altre chat: limite %d", chat_limit); }
      vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, HDR + CHIPS, SW, SH - TABH);
      clip_y0 = HDR + CHIPS; clip_y1 = SH - TABH;
      for (int v = 0; v < nvis; v++) {
        float y = HDR + CHIPS + v * ROWH - list_scroll;
        if (y + ROWH < HDR + CHIPS || y > SH) continue;
        Chat *c = &chats[visible_idx[v]];
        if (nav_mode && v == list_sel) { vita2d_draw_rectangle(0, y, SW, ROWH, RGBA8(0x2B, 0x7B, 0xFF, 64)); vita2d_draw_rectangle(0, y, 6, ROWH, RGBA8(0x2B, 0x7B, 0xFF, 255)); }   // riga selezionata: azzurro trasparente
        if (sel_find(c->jid) >= 0) vita2d_draw_rectangle(0, y, SW, ROWH, RGBA8(0x2B, 0x7B, 0xFF, 70));
        draw_avatar(c->jid, c->name, 44, y + ROWH / 2, 26);
        if (sel_find(c->jid) >= 0) { vita2d_draw_fill_circle(66, y + ROWH / 2 + 20, 11, C_BG); vita2d_draw_fill_circle(66, y + ROWH / 2 + 20, 9, C_GREEN2); tline(61, y + ROWH / 2 + 20, 65, y + ROWH / 2 + 24, C_WHITE); tline(65, y + ROWH / 2 + 24, 72, y + ROWH / 2 + 15, C_WHITE); }
        char nm[200]; fit(c->name, SW - 88 - 84, 20, nm, sizeof nm); text(88, (int)y + 10, C_TEXT, 20, nm);
        char ls[300]; fit(c->last, SW - 88 - 70, 16, ls, sizeof ls); text(88, (int)y + 38, C_SUB, 16, ls);
        char tm[16] = ""; if (c->ts) fmt_time(c->ts, tm, sizeof tm, 1);
        text(SW - 16 - tw(13, tm), (int)y + 14, c->unread > 0 ? C_GREEN2 : C_SUB, 13, tm);
        if (c->unread > 0) {
          char u[8]; snprintf(u, sizeof u, "%d", c->unread > 999 ? 999 : c->unread);
          int uw = tw(13, u) + 14; if (uw < 24) uw = 24;
          rrect(SW - 16 - uw, y + 40, uw, 24, 12, C_GREEN2); text(SW - 16 - uw + (uw - tw(13, u)) / 2, (int)y + 44, C_BG, 13, u);
        }
        if (c->pinned) { int px = SW - 28 - (c->unread > 0 ? 36 : 0); float py = y + 52; vita2d_draw_fill_circle(px, py - 4, 5, C_SUB); vita2d_draw_rectangle(px - 1, py, 2, 8, C_SUB); vita2d_draw_rectangle(px - 6, py - 2, 12, 2, C_SUB); }   // puntina: chat fissata
        btn(0, (int)y, SW, ROWH, B_ROW, visible_idx[v]);
      }
      vita2d_disable_clipping(); clip_y0 = 0; clip_y1 = SH;
      CHROME_BEGIN();
      vita2d_draw_rectangle(0, 0, SW, HDR, C_BG);
      if (nsel == 0) {
      textb(20, 14, C_GREEN2, 30, show_archived ? T("Archiviate") : "WhatsApp");
      draw_avatar(prof.jid[0] ? prof.jid : "me", prof.name, SW - 40, 30, 20); btn(SW - 70, 0, 70, HDR, B_PROFILE, 0);
      vita2d_draw_fill_circle(SW - 168, 28, 9, C_TEXT); vita2d_draw_fill_circle(SW - 168, 28, 7, C_BG); tline(SW - 162, 34, SW - 154, 42, C_TEXT); btn(SW - 195, 0, 55, HDR, B_SEARCH_ICON, 0);
      for (int d = 0; d < 3; d++) vita2d_draw_fill_circle(SW - 110, 20 + d * 10, 2.5f, C_TEXT); btn(SW - 135, 0, 55, HDR, B_LISTMENU, 0);
      } else draw_sel_bar();
      vita2d_draw_rectangle(0, HDR, SW, CHIPS, C_BG);
      const char *chn[] = { T("Tutte"), T("Non lette"), T("Gruppi") }; int cx = 16;
      for (int i = 0; i < 3; i++) {
        int w = tw(16, chn[i]) + 28; rrect(cx, HDR + 5, w, 34, 17, filter_sel == i ? C_CHIPSEL : C_CHIP);
        text(cx + 14, HDR + 11, filter_sel == i ? C_GREEN2 : C_SUB, 16, chn[i]); btn(cx, HDR, w, CHIPS, B_CHIP, i);
        cx += w + 8;
      }
      if (nchats == 0) text(60, 260, C_SUB, 20, T("Sincronizzazione chat..."));
      else if (nvis == 0) text(60, 260, C_SUB, 20, show_archived ? T("Nessuna chat archiviata") : T("Nessuna chat"));
      vita2d_draw_fill_circle(SW - 50, SH - TABH - 46, 30, C_GREEN); vita2d_draw_rectangle(SW - 62, SH - TABH - 48, 24, 4, C_WHITE); vita2d_draw_rectangle(SW - 52, SH - TABH - 58, 4, 24, C_WHITE);
      btn(SW - 85, SH - TABH - 80, 70, 70, B_FAB, 0);
      if (sync_active) {
        int sy0 = SH - TABH - 34; vita2d_draw_rectangle(0, sy0, SW, 34, C_BAR);
        float frac = sync_pct / 100.0f; vita2d_draw_rectangle(0, sy0, SW, 4, C_DIV); vita2d_draw_rectangle(0, sy0, SW * frac + (sync_pct == 0 ? 40 : 0), 4, C_GREEN2);
        char sl[160]; unsigned el = (frame_no - sync_t0) / 60;
        if (eng_busy_s > 2) snprintf(sl, sizeof sl, T("Il motore sta elaborando i dati... (%d s)"), eng_busy_s);
        else if (sync_pct >= 3) { unsigned eta = el * (100 - sync_pct) / sync_pct; snprintf(sl, sizeof sl, T("Sincronizzazione: %d%% - %d chat - restano circa %u:%02u"), sync_pct, sync_chats, eta / 60, eta % 60); }
        else snprintf(sl, sizeof sl, T("Sincronizzazione in corso... %d chat (trascorsi %u:%02u)"), sync_chats, el / 60, el % 60);
        text(16, sy0 + 9, C_TEXT, 15, sl);
      }
      draw_tabs();
      CHROME_END();
    }

    if (screen == S_CHAT && cur_chat >= 0 && cur_chat < nchats) {
      Chat *c = &chats[cur_chat];
      int maxbw = SW * 62 / 100; int topy = HDR + 4;
      int extra_bar_c = (reply_id[0] || edit_id[0]) ? 46 : 0; int extra_bar = (int)(extra_bar_c * KCH / UK + 0.5f);
      int boty = SH - BARH - extra_bar - (emoji_open ? EMO_PANEL : 0);
      int fresh = !strcmp(msgs_jid, c->jid);
      float total = 8;
      if (fresh) for (int i = 0; i < nmsgs; i++) {
        Msg *m = &msgs[i]; int sn = c->group && !m->from_me && m->name[0];
        layout_msg(m, maxbw, sn); total += m->h + (m->nreact ? 18 : 0) + 6;
      }
      if (fresh && chat_logged_frames < 3) { chat_logged_frames++; crumb("chat: layout %d messaggi ok, altezza %.0f (frame %d)", nmsgs, total, chat_logged_frames); }
      if (fresh && anchor_id[0]) {
        int ai = -1; float off = 0;
        for (int i = 0; i < nmsgs; i++) { if (!strcmp(msgs[i].id, anchor_id)) { ai = i; break; } off += msgs[i].h + (msgs[i].nreact ? 18 : 0) + 6; }
        if (ai > 0) { chat_scroll += off; anchor_id[0] = 0; older_pending = 0; crumb("messaggi piu' vecchi caricati: +%d (totale %d)", ai, nmsgs); }
        else if (msgs_gen >= gen_req + 8) { anchor_id[0] = 0; older_pending = 0; no_more_older = 1; crumb("nessun messaggio piu' vecchio"); }
      }
      float viewh = boty - topy; float maxs = total > viewh ? total - viewh : 0;
      if (fresh && !older_pending && !no_more_older && !chat_stick && chat_scroll < 60 && nmsgs >= msg_limit && msg_limit < 300 && nmsgs > 0) {
        snprintf(anchor_id, sizeof anchor_id, "%s", msgs[0].id); gen_req = msgs_gen; older_pending = 1; msg_limit += 30; chat_vel = 0; crumb("carico messaggi piu' vecchi: limite %d", msg_limit);
      }
      static int last_n = -1; if (nmsgs != last_n) { last_n = nmsgs; if (chat_scroll >= maxs - 60 || chat_stick) chat_stick = 1; }
      if (chat_stick) chat_scroll = maxs;
      if (chat_scroll < 0) { chat_scroll = 0; chat_vel = 0; } if (chat_scroll >= maxs) { chat_scroll = maxs; chat_vel = 0; if (!touching) chat_stick = 1; }
      vita2d_draw_rectangle(0, topy, SW, boty - topy, (ui_dark ? THEMES : THEMES_L)[theme_idx]);
      vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, topy, SW, boty); clip_y0 = topy; clip_y1 = boty;
      float y = topy + 8 - chat_scroll;
      if (fresh) for (int i = 0; i < nmsgs; i++) {
        Msg *m = &msgs[i]; int sn = c->group && !m->from_me && m->name[0];
        float slot = m->h + (m->nreact ? 18 : 0);
        if (y + slot >= topy && y <= boty) {
          float bx = m->from_me ? SW - 16 - m->w : 16;
          int sticker = m->kind == K_STICKER;
          if (!sticker) rrect(bx, y, m->w, m->h, 10, m->from_me ? C_OUT : (m->kind == K_DELETED ? C_BG : C_IN));
          btn((int)bx, (int)y, m->w, m->h, B_MSG, i);     // sotto: i pulsanti piu' specifici (immagine, documento) registrati dopo hanno la precedenza
          float cy = y + PADY; int pad = has_media(m) ? 4 : PADX; float cx0 = bx + pad;
          if (sn) { text((int)bx + PADX, (int)cy - 1, hash_col(m->name), 15, m->name); cy += HNAME; }
          if (m->hasq) {
            rrect(cx0, cy, m->w - 2 * pad, HQUOTE, 6, C_QUOTE); vita2d_draw_rectangle(cx0, cy, 4, HQUOTE, m->from_me ? C_GREEN2 : C_BLUE);
            char qn[100]; fit(m->qname, m->w - 2 * pad - 20, 14, qn, sizeof qn); text((int)cx0 + 10, (int)cy + 4, m->from_me ? C_GREEN2 : C_BLUE, 14, qn);
            char qt[200]; fit(m->qtext, m->w - 2 * pad - 20, 15, qt, sizeof qt); text((int)cx0 + 10, (int)cy + 24, C_SUB, 15, qt);
            cy += HQUOTE + 4;
          }
          if (has_media(m)) {
            Media *md = media_get(c->jid, m->id);
            if (md && md->tex) { float sx = (float)m->mw / vita2d_texture_get_width(md->tex), sy = (float)m->mh / vita2d_texture_get_height(md->tex); vita2d_draw_texture_scale(md->tex, cx0, cy, sx, sy); }
            else { rrect(cx0, cy, m->mw, m->mh, 6, RGBA8(0, 0, 0, 90)); text((int)cx0 + m->mw / 2 - 30, (int)cy + m->mh / 2 - 10, C_SUB, 18, md && md->state == 2 ? T("Errore") : "..."); }
            if (m->kind == K_VIDEO) { vita2d_draw_fill_circle(cx0 + m->mw / 2, cy + m->mh / 2, 26, RGBA8(0, 0, 0, 150)); text((int)cx0 + m->mw / 2 - 7, (int)cy + m->mh / 2 - 13, C_WHITE, 26, ">"); }
            btn((int)cx0, (int)cy, m->mw, m->mh, B_MSG, i + 1000);
            cy += m->mh + 4;
          }
          if (m->kind == K_DOC) {
            rrect(cx0, cy, m->w - 2 * pad, HDOC, 8, C_QUOTE);
            rrect(cx0 + 8, cy + 8, 34, 40, 5, C_RED); text((int)cx0 + 12, (int)cy + 20, C_WHITE, 14, "DOC");
            char fn[160]; fit(m->fname, m->w - 2 * pad - 70, 16, fn, sizeof fn); text((int)cx0 + 52, (int)cy + 8, C_TEXT, 16, fn);
            char sz[60]; snprintf(sz, sizeof sz, T("%.1f KB - tocca per scaricare"), m->size / 1024.0); text((int)cx0 + 52, (int)cy + 30, C_SUB, 13, sz);
            btn((int)cx0, (int)cy, m->w - 2 * pad, HDOC, B_MSG, i + 1000); cy += HDOC + 4;
          }
          if (m->kind == K_VOICE || m->kind == K_AUDIO) {
            int apct = 0, asec = 0, ast = aud_state(chats[cur_chat].jid, m->id, &apct, &asec);   // 0 fermo, 1 preparo, 2 in riproduzione, 3 errore
            vita2d_draw_fill_circle(cx0 + 20, cy + 21, 18, ast == 3 ? C_RED : C_GREEN); text((int)cx0 + 15, (int)cy + 8, C_WHITE, 22, ast == 2 ? "||" : ast == 1 ? "..." : ">");
            { float tw_ = m->w - 2 * pad - 60; vita2d_draw_rectangle(cx0 + 50, cy + 20, tw_, 3, C_SUB); if (ast == 2 && m->dur > 0) { float f = (float)asec / m->dur; if (f > 1) f = 1; vita2d_draw_rectangle(cx0 + 50, cy + 20, tw_ * f, 3, C_GREEN); } else if (ast == 1) vita2d_draw_rectangle(cx0 + 50, cy + 20, tw_ * apct / 100.0f, 3, C_GREEN); }
            char du[24]; snprintf(du, sizeof du, "%d:%02d", m->dur / 60, m->dur % 60); text((int)cx0 + 50, (int)cy + 26, C_SUB, 13, du);
            btn((int)cx0, (int)cy, m->w - 2 * pad, HVOICE, B_MSG, i + 1000); cy += HVOICE + 2;   // tocco = play/stop
          }
          for (int k = 0; k < m->nlines; k++) text((int)(bx + pad), (int)(cy + k * LH), m->kind == K_DELETED ? C_SUB : C_TEXT, BFONT, m->lines[k]);
          // ora + spunte
          char tm[16]; fmt_time(m->ts, tm, sizeof tm, 0);
          int overlay = has_media(m) && m->nlines == 0;
          int rw = tw(12, tm) + (m->from_me ? 24 : 0); int ew = m->edited ? tw(12, T("Modificato ")) : 0;
          float tx = bx + m->w - pad - 4 - rw - ew, ty = y + m->h - 20;
          if (sticker) { tx = bx + m->w - rw - 4 - ew; ty = y + m->h - 17; }
          if (overlay && !sticker) rrect(tx - 6, ty - 2, rw + ew + 10, 18, 9, RGBA8(40, 40, 40, 255));
          unsigned tcol = overlay && !sticker ? C_WHITE : C_SUB;
          if (m->edited) text((int)tx, (int)ty, tcol, 12, T("Modificato"));
          text((int)tx + ew, (int)ty, tcol, 12, tm);
          if (m->from_me) { unsigned kc = m->status >= 4 ? C_BLUE : tcol; check(tx + ew + tw(12, tm) + 6, ty + 3, kc, m->status >= 3); }
          if (m->nreact) {
            int rx = (int)(m->from_me ? bx + m->w - 20 : bx + 10);
            for (int k = 0; k < m->nreact; k++) {
              char rt[24]; if (m->react[k].n > 1) snprintf(rt, sizeof rt, "%s %d", m->react[k].e, m->react[k].n); else snprintf(rt, sizeof rt, "%s", m->react[k].e);
              int w = tw(14, rt) + 14; int rxx = m->from_me ? rx - w : rx; rrect(rxx, y + m->h - 6, w, 24, 12, C_BAR); text(rxx + 7, (int)y + m->h - 3, C_TEXT, 14, rt); rx += m->from_me ? -(w + 4) : (w + 4);
            }
          }
        }
        y += slot + 6;
      }
      vita2d_disable_clipping(); clip_y0 = 0; clip_y1 = SH;
      if (!fresh) text(SW / 2 - 60, 260, C_SUB, 18, T("Caricamento..."));
      // header (barra fissa)
      CHROME_BEGIN();
      vita2d_draw_rectangle(0, 0, SW, HDR, C_BAR);
      back_arrow(); btn(0, 0, 70, HDR, B_BACK, 0);
      draw_avatar(c->jid, c->name, 86, 30, 20);
      char nm[200]; fit(c->name, SW - 210, 21, nm, sizeof nm); textb(120, 8, C_TEXT, 21, nm); btn(70, 0, SW - 150, HDR, B_HEADER_INFO, 0);
      for (int d = 0; d < 3; d++) vita2d_draw_fill_circle(SW - 36, 20 + d * 10, 2.5f, C_TEXT); btn(SW - 75, 0, 75, HDR, B_CHATMENU, 0);
      const char *sub = !strcmp(pst, "composing") ? T("sta scrivendo...") : !strcmp(pst, "recording") ? T("sta registrando un audio...") : !strcmp(pst, "available") ? "online" : "";
      if (sub[0]) text(120, 34, C_GREEN2, 14, sub);
      // reply/edit banner
      int by = SH - BARH - extra_bar_c - (emoji_open ? EMO_PANEL : 0);
      if (extra_bar_c) {
        vita2d_draw_rectangle(0, by, SW, extra_bar_c, C_BAR);
        vita2d_draw_rectangle(14, by + 6, 4, 34, edit_id[0] ? C_GREEN2 : C_BLUE);
        text(28, by + 5, edit_id[0] ? C_GREEN2 : C_BLUE, 14, edit_id[0] ? T("Modifica messaggio") : (reply_name[0] ? reply_name : T("Risposta")));
        if (!edit_id[0]) { char rt[200]; fit(reply_text, SW - 140, 15, rt, sizeof rt); text(28, by + 24, C_SUB, 15, rt); }
        text(SW - 44, by + 8, C_SUB, 24, "x"); btn(SW - 70, by, 70, extra_bar_c, B_CANCEL_REPLY, 0);
      }
      // pannello emoji
      if (emoji_open) {
        int py = SH - BARH - EMO_PANEL;
        vita2d_draw_rectangle(0, py, SW, EMO_PANEL, C_BAR);
        text(24, py + 8, C_SUB, 16, T("Emoji")); rrect(SW - 160, py + 4, 70, 32, 8, C_CHIP); text(SW - 142, py + 9, C_TEXT, 18, "<x"); btn(SW - 160, py, 70, 40, B_EMO_BACK, 0);
        rrect(SW - 80, py + 4, 70, 32, 8, C_CHIP); text(SW - 66, py + 9, C_TEXT, 18, "ABC"); btn(SW - 80, py, 80, 40, B_EMO_CLOSE, 0);
        int cell = 56, cols = (SW - 40) / cell; if (cols < 4) cols = 4; int rows = (emo_n + cols - 1) / cols; float gmax = rows * cell - (EMO_PANEL - 40); if (gmax < 0) gmax = 0;
        if (emo_scroll < 0) { emo_scroll = 0; emo_vel = 0; } if (emo_scroll > gmax) { emo_scroll = gmax; emo_vel = 0; }
        vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, py + 40, SW, py + EMO_PANEL); clip_y0 = py + 40; clip_y1 = py + EMO_PANEL;
        for (int r = 0; r < rows; r++) {
          float ry = py + 40 + r * cell - emo_scroll; if (ry + cell < py + 40 || ry > py + EMO_PANEL) continue;
          for (int k = 0; k < cols; k++) {
            int idx = r * cols + k; if (idx >= emo_n) break;
            int ex = (SW - cols * cell) / 2 + k * cell; float sc = 40.0f / 32.0f;
            vita2d_draw_texture_part_scale(emo_tex, ex + 8, floorf(ry + 8), (idx % 32) * 32 + 0.5f, (idx / 32) * 32 + 0.5f, 31, 31, sc * 32 / 31, sc * 32 / 31);
            btn(ex, (int)ry, cell, cell, B_EMO_CELL, idx);
          }
        }
        vita2d_disable_clipping(); clip_y0 = 0; clip_y1 = SH;
      }
      // barra input
      int iy = SH - BARH;
      vita2d_draw_rectangle(0, iy, SW, BARH, C_BG);
      rrect(12, iy + 8, SW - 12 - 80, 48, 24, C_BAR);
      unsigned ec = emoji_open ? C_GREEN2 : C_SUB;
      vita2d_draw_fill_circle(44, iy + 32, 13, ec); vita2d_draw_fill_circle(44, iy + 32, 11, C_BAR);
      vita2d_draw_fill_circle(40, iy + 28, 2, ec); vita2d_draw_fill_circle(48, iy + 28, 2, ec); vita2d_draw_rectangle(39, iy + 36, 10, 2, ec);
      btn(20, iy, 52, BARH, B_EMOJI_BTN, 0);
      text(SW - 148, iy + 16, C_SUB, 30, "+"); btn(SW - 170, iy, 76, BARH, B_ATTACH, 0);
      if (draft[0]) {
        const char *tail = draft; const char *nl = strrchr(draft, '\n'); if (nl) tail = nl + 1;
        char shown[400];
        if (tw(19, tail) <= SW - 290) snprintf(shown, sizeof shown, "%s", tail);
        else { const char *p = tail + strlen(tail); while (p > tail) { const char *q = p - 1; while (q > tail && (*q & 0xC0) == 0x80) q--; if (tw(19, q) > SW - 290) break; p = q; } snprintf(shown, sizeof shown, "%s", p); }
        text(84, iy + 21, C_TEXT, 19, shown);
      } else text(84, iy + 21, C_SUB, 19, T("Scrivi un messaggio"));
      btn(76, iy, SW - 76 - 180, BARH, B_DRAFT, 0);
      vita2d_draw_fill_circle(SW - 40, iy + 32, 26, draft[0] ? C_GREEN : C_BAR); send_arrow(SW - 40, iy + 32);
      btn(SW - 70, iy, 70, BARH, B_SEND, 0);
      if (c->channel) { vita2d_draw_rectangle(0, iy, SW, BARH, C_BAR); text((SW - tw(18, T("Canale: sola lettura"))) / 2, iy + 22, C_SUB, 18, T("Canale: sola lettura")); btn(0, iy, SW, BARH, B_NONE, 0); }
      else if (c->blocked) { vita2d_draw_rectangle(0, iy, SW, BARH, C_BAR); text((SW - tw(18, T("Hai bloccato questo contatto. Tocca per sbloccare."))) / 2, iy + 22, C_SUB, 18, T("Hai bloccato questo contatto. Tocca per sbloccare.")); btn(0, iy, SW, BARH, B_UNBLOCK_BAR, 0); }

      CHROME_END();
      if (menu_open && !viewer_open) {
        float uk_m = UK; UK = fminf(UK, 1.15f);   // il menu ha molte voci: scala ridotta
        Msg *mm = find_msg(menu_id);
        if (!mm) menu_open = 0; else {
          btn_reset();
          vita2d_draw_rectangle(0, 0, SW, SH, RGBA8(0, 0, 0, 150)); btn(0, 0, SW, SH, B_MENU_CLOSE, 0);
          int mw = 380, mx = (SW - mw) / 2;
          int own_text = mm->from_me && mm->kind == K_TEXT && now_unix() - mm->ts < 15 * 60;
          const char *items[7]; int ids[7]; int ni = 0;
          items[ni] = T("Rispondi"); ids[ni++] = 0; items[ni] = T("Copia nella bozza"); ids[ni++] = 1;
          if (own_text) { items[ni] = T("Modifica"); ids[ni++] = 2; }
          if (mm->from_me && mm->kind != K_DELETED) { items[ni] = T("Elimina per tutti"); ids[ni++] = 3; }
          items[ni] = T("Elimina per me"); ids[ni++] = 4;
          int mh = 70 + ni * 50, my = (SH - mh) / 2;
          rrect(mx, my, mw, mh, 14, C_BAR); btn(mx, my, mw, mh, B_NONE, 0);
          for (int k = 0; k < 6; k++) { int ex = mx + 20 + k * 58; emoji_cp(QUICK[k], ex, my + 14, 32); btn(ex - 6, my + 8, 56, 52, B_MENU_REACT, k); }
          for (int k = 0; k < ni; k++) { int iy2 = my + 70 + k * 50; vita2d_draw_rectangle(mx + 16, iy2, mw - 32, 1, C_DIV); text(mx + 28, iy2 + 12, ids[k] == 3 ? C_RED : C_TEXT, 20, items[k]); btn(mx, iy2, mw, 50, B_MENU_ITEM, ids[k]); }
        }
        UK = uk_m;
      }
    }

    if (screen == S_PROFILE) {
      vita2d_draw_rectangle(0, 0, SW, 60, C_BAR); back_arrow(); btn(0, 0, 70, 60, B_BACK, 0);
      textb(70, 14, C_TEXT, 26, T("Profilo"));
      { const char *ab_ = T("Informazioni"); int abw = tw(18, ab_) + 56; rrect(SW - abw - 20, 10, abw, 40, 20, C_GREEN2); text(SW - abw - 20 + 28, 20, RGBA8(0, 0, 0, 255), 18, ab_); btn(SW - abw - 20, 10, abw, 40, B_ABOUT, 0); }   // in alto a destra: ben visibile
      draw_avatar(prof.jid[0] ? prof.jid : "me", prof.name, 200, 250, 90);
      vita2d_draw_fill_circle(280, 330, 24, C_GREEN); text(271, 312, C_WHITE, 30, "+"); btn(100, 160, 200, 200, B_PROF_PHOTO, 0);
      text(110, 372, C_SUB, 14, T("Tocca per cambiare foto"));
      struct { const char *lab; const char *val; int id; } rows[3] = { { T("Nome"), prof.name, B_PROF_NAME }, { T("Info"), prof.about, B_PROF_ABOUT }, { T("Telefono"), prof.phone, B_NONE } };
      for (int i = 0; i < 3; i++) {
        int ry = 100 + i * 110; text(380, ry, C_SUB, 16, rows[i].lab);
        char v[300]; fit(rows[i].val[0] ? rows[i].val : (rows[i].id == B_PROF_ABOUT ? T("(vuota)") : "-"), 480, 22, v, sizeof v); text(380, ry + 26, C_TEXT, 22, v);
        if (rows[i].id != B_NONE) { text(SW - 100, ry + 14, C_GREEN2, 16, T("Modifica")); btn(360, ry - 10, SW - 380, 90, rows[i].id, 0); }
        vita2d_draw_rectangle(380, ry + 70, SW - 420, 1, C_DIV);
      }
      if (!prof.ok) text(380, 440, C_SUB, 16, T("Caricamento profilo..."));
      { UpdInfo ui_; upd_snapshot(&ui_); char vt[96]; snprintf(vt, sizeof vt, T("Versione %s"), ui_.current);
        rrect(100, 400, 250, 48, 24, C_BAR); text(100 + (250 - tw(17, T("Controlla aggiornamenti"))) / 2, 413, C_GREEN2, 17, T("Controlla aggiornamenti")); btn(100, 400, 250, 48, B_UPD_CHECK, 0);
        text(100 + (250 - tw(14, vt)) / 2, 455, C_SUB, 14, vt);
        char lt[128]; snprintf(lt, sizeof lt, T("Lingua: %s"), i18n_native_name(i18n_lang())); rrect(100, 482, 250, 40, 20, C_BAR); text(100 + (250 - tw(16, lt)) / 2, 492, C_TEXT, 16, lt); btn(100, 482, 250, 40, B_LANG, 0); }
      rrect(380, 400, 240, 48, 24, C_BAR); text(380 + (240 - tw(19, T("Disconnetti"))) / 2, 412, C_RED, 19, T("Disconnetti")); btn(380, 400, 240, 48, B_LOGOUT, 0);
      if (plugin_cache < 0 || frame_no % 120 == 0) plugin_cache = plugin_installed();
      const char *nl = !plugin_cache ? T("Installa notifiche") : notif_enabled() ? T("Notifiche: attive") : T("Notifiche: disattivate");
      rrect(640, 400, 280, 48, 24, C_BAR); text(640 + (280 - tw(19, nl)) / 2, 412, plugin_cache && notif_enabled() ? C_GREEN2 : C_SUB, 19, nl); btn(640, 400, 280, 48, B_PLUGIN, 0);
      { const char *tl = ui_dark ? T("Tema: scuro") : T("Tema: chiaro"); rrect(380, 458, 240, 40, 20, C_BAR); text(380 + (240 - tw(16, tl)) / 2, 468, C_TEXT, 16, tl); btn(380, 458, 240, 40, B_THEME_UI, 0); }
      { // stato del plugin: attivo se ha aggiornato il suo file da meno di 40 secondi
        int al = alive_state();
        const char *st = !plugin_cache ? T("Plugin notifiche: non installato") : al == 1 ? T("Plugin notifiche: ATTIVO") : al < 0 ? T("Plugin notifiche: verifica in corso...") : T("Plugin notifiche: installato ma non attivo (riavvia la Vita)");
        text(380, 504, (al == 1 && plugin_cache) ? C_GREEN2 : C_SUB, 14, st); btn(380, 498, 250, 32, B_PLUGIN_DIAG, 0);   // toccando la riga: diagnostica
      }
      if (plugin_cache) { rrect(640, 458, 280, 40, 20, C_BAR); text(640 + (280 - tw(16, T("Rimuovi plugin notifiche"))) / 2, 468, C_RED, 16, T("Rimuovi plugin notifiche")); btn(640, 458, 280, 40, B_PLUGIN_RM, 0); }
    }

    if (screen == S_PICKER) {
      vita2d_draw_rectangle(0, 0, SW, 60, C_BAR); back_arrow(); btn(0, 0, 70, 60, B_BACK, 0);
      char hd[300]; fit(pick_dir[0] ? pick_dir : (pick_mode == 1 ? T("Scegli una foto") : T("Scegli un file")), 640, 22, hd, sizeof hd); textb(70, 16, C_TEXT, 22, hd);
      rrect(SW - 190, 8, 174, 44, 22, C_GREEN2); text(SW - 190 + (174 - tw(18, T("Fotocamera"))) / 2, 20, C_WHITE, 18, T("Fotocamera")); btn(SW - 190, 8, 174, 44, B_CAM_OPEN, 0);
      float viewh = SH - 60; float total = (nents + (pick_dir[0] ? 1 : 0)) * 52.0f; float maxs = total > viewh ? total - viewh : 0;
      if (pick_scroll < 0) { pick_scroll = 0; pick_vel = 0; } if (pick_scroll > maxs) { pick_scroll = maxs; pick_vel = 0; }
      vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, 60, SW, SH); clip_y0 = 60;
      int row = 0;
      if (pick_dir[0]) { float ry = 60 + row * 52 - pick_scroll; text(40, (int)ry + 12, C_GREEN2, 20, ".. (su)"); btn(0, (int)ry, SW, 52, B_PICK_UP, 0); row++; }
      for (int i = 0; i < nents; i++, row++) {
        float ry = 60 + row * 52 - pick_scroll; if (ry + 52 < 60 || ry > SH) continue;
        if (ents[i].dir) rrect(30, ry + 14, 30, 24, 4, RGBA8(0xF5, 0xB6, 0x4A, 255)); else rrect(36, ry + 10, 20, 32, 3, C_SUB);
        char nm[200]; fit(ents[i].name, 780, 20, nm, sizeof nm); text(76, (int)ry + 12, C_TEXT, 20, nm);
        vita2d_draw_rectangle(76, ry + 51, SW - 90, 1, C_DIV); btn(0, (int)ry, SW, 52, B_PICK_ENTRY, i);
      }
      vita2d_disable_clipping(); clip_y0 = 0;
    }

    if (screen == S_PREVIEW) {
      vita2d_draw_rectangle(0, 0, SW, 60, C_BAR); back_arrow(); btn(0, 0, 70, 60, B_PREVIEW_CANCEL, 0);
      char hd[300]; fit(base_name(sel_path), 800, 20, hd, sizeof hd); textb(70, 18, C_TEXT, 20, hd);
      if (prev_tex) { float w0 = vita2d_texture_get_width(prev_tex), h0 = vita2d_texture_get_height(prev_tex); float s = fminf((SW - 40) / w0, (SH - (pick_mode == 1 ? 150 : 200)) / h0); float ix = (SW - w0 * s) / 2, iy2 = 70 + (SH - (pick_mode == 1 ? 150 : 200) - h0 * s) / 2; vita2d_draw_texture_scale(prev_tex, ix, iy2, s, s);
        if (pick_mode == 1) { float side = fminf(w0, h0) * s, cx0 = ix + (w0 * s - side) / 2, cy0 = iy2 + (h0 * s - side) / 2; unsigned fc = RGBA8(0x25, 0xD3, 0x66, 255);
          vita2d_draw_rectangle(cx0, cy0, side, 3, fc); vita2d_draw_rectangle(cx0, cy0 + side - 3, side, 3, fc); vita2d_draw_rectangle(cx0, cy0, 3, side, fc); vita2d_draw_rectangle(cx0 + side - 3, cy0, 3, side, fc);
          text(SW / 2 - tw(15, T("La foto verra' ritagliata nel riquadro verde (quadrata, al centro)")) / 2, SH - 78, C_SUB, 15, T("La foto verra' ritagliata nel riquadro verde (quadrata, al centro)")); } }
      else { rrect(SW / 2 - 60, 160, 120, 150, 10, C_BAR); text(SW / 2 - 30, 215, C_SUB, 28, strcmp(sel_kind, "doc") ? sel_kind : "DOC"); }
      if (pick_mode != 1) {
        rrect(14, SH - 64, SW - 100, 48, 24, C_BAR);
        char cp[300]; fit(caption[0] ? caption : T("Aggiungi una didascalia..."), SW - 160, 19, cp, sizeof cp); text(36, SH - 50, caption[0] ? C_TEXT : C_SUB, 19, cp); btn(14, SH - 64, SW - 100, 56, B_PREVIEW_CAPTION, 0);
      }
      vita2d_draw_fill_circle(SW - 40, SH - 40, 26, C_GREEN); send_arrow(SW - 40, SH - 40);
      btn(SW - 70, SH - 64, 70, 60, B_PREVIEW_SEND, 0);
    }

    FC("schermata base disegnata");
    draw_extra_screens(); FC("popup/extra disegnati");

    if (toast_frames > 0) { toast_frames--; int w = tw(17, toast_show) + 40; rrect((SW - w) / 2, SH - 120, w, 40, 20, RGBA8(0x30, 0x3C, 0x44, 245)); text((SW - w) / 2 + 20, SH - 111, C_TEXT, 17, toast_show); }
    { unsigned fs = vita2d_pool_free_space(); unsigned used = pool_total > fs ? pool_total - fs : 0; if (used > pool_max_used) { if (used > pool_max_used + pool_max_used / 4 + 20000) crumb("gpu: memoria temporanea usata %u KB di %u KB", used / 1024, pool_total / 1024); pool_max_used = used; } }
    vita2d_end_drawing(); FC("end_drawing ok");
    {   // solo per i test: se esiste ux0:data/whatsapp/shot_at.txt (numero di frame), a quel frame salva lo schermo in shot.ppm
      static int shot_frame = -1; if (shot_frame < 0) { char b[16] = ""; SceUID sf = sceIoOpen("ux0:data/whatsapp/shot_at.txt", SCE_O_RDONLY, 0); if (sf >= 0) { int n = sceIoRead(sf, b, 15); sceIoClose(sf); b[n > 0 ? n : 0] = 0; } shot_frame = atoi(b); }
      if (shot_frame > 0 && (int)frame_no == shot_frame) {
        vita2d_wait_rendering_done(); const unsigned char *fb = vita2d_get_current_fb(); SceUID of = sceIoOpen("ux0:data/whatsapp/shot.ppm", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (fb && of >= 0) { sceIoWrite(of, "P6\n960 544\n255\n", 15); static unsigned char row[960 * 3]; for (int y = 0; y < 544; y++) { for (int x = 0; x < 960; x++) { const unsigned char *p = fb + ((size_t)y * 960 + x) * 4; row[x * 3] = p[0]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[2]; } sceIoWrite(of, row, sizeof row); } crumb("schermo salvato in shot.ppm (frame %d)", (int)frame_no); }
        if (of >= 0) sceIoClose(of);
      } }
    vita2d_common_dialog_update(); FC("dialog_update ok"); vita2d_swap_buffers(); FC("swap ok");

    { static int last_screen = -1; if (screen != last_screen) { crumb("schermata %d", screen);
        if (last_screen == S_CHAT && screen == S_LIST && cur_chat >= 0) for (int v = 0; v < nvis; v++) if (visible_idx[v] == cur_chat) { list_sel = v; list_sel_dirty = 1; break; }
        last_screen = screen; } }
    // ---- risoluzione input ----
    Btn *hit = (tap || lp) ? btn_at(tapx, tapy) : NULL;
    int id = hit ? hit->id : B_NONE, arg = hit ? hit->arg : 0;
    if (tap || lp) { crumb("%s id=%d arg=%d schermata=%d", lp ? "pressione lunga" : "tocco", id, arg, screen); }
    if (lp) {
      if (id == B_ROW && screen == S_LIST && arg >= 0 && arg < nchats && !popup_open) sel_toggle(chats[arg].jid);   // pressione lunga su una chat: la seleziona
      if (id == B_MSG && arg < 1000 && arg < nmsgs && !menu_open && !viewer_open && !(cur_chat >= 0 && cur_chat < nchats && chats[cur_chat].channel)) { menu_open = 1; snprintf(menu_id, sizeof menu_id, "%s", msgs[arg].id); }
    } else if (tap) switch (id) {
      case B_ROW: if (nsel > 0 && arg >= 0 && arg < nchats) sel_toggle(chats[arg].jid); else open_chat(arg); break;
      case B_CHIP: filter_sel = arg; list_scroll = 0; break;
      case B_PROFILE: screen = S_PROFILE; break;
      case B_BACK: if (extra_back()) break; if (screen == S_CHAT) close_chat(); else if (screen == S_PROFILE) screen = S_LIST; else if (screen == S_PICKER) { if (pick_dir[0]) pick_up(); else screen = (pick_mode == 1 ? S_PROFILE : pick_mode == 2 ? S_UPDATES : S_CHAT); } break;
      case B_MSG: if (arg >= 1000 && arg - 1000 < nmsgs) { Msg *m = &msgs[arg - 1000]; if ((m->kind == K_DOC) && cur_chat >= 0) start_prep(chats[cur_chat].jid, m->id, "file", m->fname); else if ((m->kind == K_AUDIO || m->kind == K_VOICE) && cur_chat >= 0) aud_toggle(chats[cur_chat].jid, m->id); else { viewer_open = 1; view_is_video = m->kind == K_VIDEO; snprintf(view_key_id, sizeof view_key_id, "%s", m->id); snprintf(view_jid, sizeof view_jid, "%s", chats[cur_chat].jid); } } break;
      case B_VIEW_CLOSE: if (vz > 1.01f || vz < 0.99f) { vz = 1; vpx = vpy = 0; } else viewer_open = 0; break;
      case B_MENU_CLOSE: menu_open = 0; break;
      case B_MENU_REACT: { Msg *m = find_msg(menu_id); if (m && cur_chat >= 0) { Job *j = job_new(J_REACT, chats[cur_chat].jid); if (j) { snprintf(j->id, sizeof j->id, "%s", m->id); char tmp[16] = ""; append_cp(tmp, sizeof tmp, QUICK[arg]); snprintf(j->text, sizeof j->text, "%s", tmp); job_commit(); } } menu_open = 0; break; }
      case B_MENU_ITEM: { Msg *m = find_msg(menu_id); if (m && cur_chat >= 0) {
          if (arg == 0) { snprintf(reply_id, sizeof reply_id, "%s", m->id); edit_id[0] = 0; snprintf(reply_name, sizeof reply_name, "%s", m->from_me ? T("Tu") : (m->name[0] ? m->name : chats[cur_chat].name)); char t[200]; if (m->kind == K_TEXT) snprintf(t, sizeof t, "%s", m->text); else snprintf(t, sizeof t, "%s", m->kind == K_IMAGE ? T("Foto") : m->kind == K_VIDEO ? T("Video") : m->kind == K_DOC ? m->fname : m->kind == K_STICKER ? T("Sticker") : T("Audio")); snprintf(reply_text, sizeof reply_text, "%s", t); for (char *p = reply_text; *p; p++) if (*p == '\n') *p = ' '; }
          else if (arg == 1) { size_t l = strlen(draft); snprintf(draft + l, sizeof draft - l, "%s", m->text); }
          else if (arg == 2) { snprintf(edit_id, sizeof edit_id, "%s", m->id); reply_id[0] = 0; snprintf(draft, sizeof draft, "%s", m->text); }
          else if (arg == 4) { char b[300]; snprintf(b, sizeof b, "{\"jid\":\"%s\",\"id\":\"%s\"}", chats[cur_chat].jid, m->id); fire("/deleteforme", b, T("Messaggio eliminato")); }
          else if (arg == 3) { Job *j = job_new(J_DELETE, chats[cur_chat].jid); if (j) { snprintf(j->id, sizeof j->id, "%s", m->id); job_commit(); } }
        } menu_open = 0; break; }
      case B_CANCEL_REPLY: if (edit_id[0]) draft[0] = 0; reply_id[0] = 0; edit_id[0] = 0; break;
      case B_EMOJI_BTN: emoji_open = !emoji_open; break;
      case B_EMO_CLOSE: emoji_open = 0; break;
      case B_EMO_BACK: backspace_draft(); break;
      case B_EMO_CELL: if (arg >= 0 && arg < emo_n) append_cp(draft, sizeof draft, emo_cp[arg]); break;
      case B_DRAFT: ime_open(IME_DRAFT, T("Scrivi un messaggio"), draft); break;
      case B_SEND: send_draft(); break;
      case B_ATTACH: pick_mode = 0; screen = S_PICKER; pick_load(""); break;
      case B_PICK_UP: pick_up(); break;
      case B_PICK_ENTRY: {
        Entry *e = &ents[arg]; char p[300]; pick_join(p, sizeof p, e->name);
        if (e->dir) pick_load(p);
        else {
          snprintf(sel_path, sizeof sel_path, "%s", p);
          snprintf(sel_kind, sizeof sel_kind, "%s", pick_mode >= 1 ? "image" : kind_for(e->name)); caption[0] = 0;
          if (prev_tex) { tex_retire(prev_tex); prev_tex = NULL; }
          if (!strcmp(sel_kind, "image")) { size_t l; unsigned char *b = read_file(sel_path, &l); if (b) { prev_tex = (l > 8 && b[0] == 0x89) ? vita2d_load_PNG_buffer(b) : decode_jpeg(b, l, 960, 0); free(b); } }
          screen = S_PREVIEW;
        }
        break; }
      case B_PREVIEW_CAPTION: ime_open(IME_CAPTION, T("Didascalia"), caption); break;
      case B_PREVIEW_CANCEL: screen = S_PICKER; break;
      case B_PREVIEW_SEND:
        if (pick_mode == 2) { Job *j = job_new(J_MEDIA, ""); if (j) { snprintf(j->path, sizeof j->path, "%s", sel_path); snprintf(j->kind, sizeof j->kind, "status"); snprintf(j->text, sizeof j->text, "%s", caption); job_commit(); toast_set(T("Pubblico la storia...")); refresh_my_at = frame_no + 300; } screen = S_UPDATES; }
        else if (pick_mode == 1) { Job *j = job_new(J_PHOTO, prof.jid); if (j) { snprintf(j->path, sizeof j->path, "%s", sel_path); job_commit(); toast_set(T("Caricamento foto...")); } screen = S_PROFILE; }
        else if (cur_chat >= 0) { Job *j = job_new(J_MEDIA, chats[cur_chat].jid); if (j) { snprintf(j->path, sizeof j->path, "%s", sel_path); snprintf(j->kind, sizeof j->kind, "%s", sel_kind); snprintf(j->text, sizeof j->text, "%s", caption); snprintf(j->id, sizeof j->id, "%s", reply_id); job_commit(); toast_set(T("Invio allegato...")); reply_id[0] = 0; chat_stick = 1; } screen = S_CHAT; }
        break;
      case B_PROF_NAME: ime_open(IME_NAME, T("Il tuo nome"), prof.name); break;
      case B_PROF_ABOUT: popup_open_about(); break;
      case B_PROF_PHOTO: pick_mode = 1; screen = S_PICKER; pick_load(""); break;
      default: handle_extra(id, arg); break;
    }

    // tasti fisici
    if (!ime_active) {
      if (pressed & SCE_CTRL_CIRCLE) {
        if (viewer_open) viewer_open = 0; else if (popup_open) popup_open = 0; else if (menu_open) menu_open = 0; else if (emoji_open) emoji_open = 0; else if (extra_back()) {}
        else if (screen == S_CHAT) close_chat(); else if (screen == S_PROFILE) screen = S_LIST;
        else if (screen == S_PICKER) { if (pick_dir[0]) pick_up(); else screen = (pick_mode == 1 ? S_PROFILE : pick_mode == 2 ? S_UPDATES : S_CHAT); }
        else if (screen == S_PREVIEW) screen = S_PICKER;
      }
      int ch_ro = screen == S_CHAT && cur_chat >= 0 && cur_chat < nchats && chats[cur_chat].channel;
      if ((pressed & SCE_CTRL_CROSS) && screen == S_CHAT && !ch_ro && !menu_open && !viewer_open) ime_open(IME_DRAFT, T("Scrivi un messaggio"), draft);
      if ((pressed & SCE_CTRL_CROSS) && screen == S_LIST && !popup_open && !menu_open) { if (nav_mode && nvis > 0 && list_sel < nvis) { if (nsel > 0) sel_toggle(chats[visible_idx[list_sel]].jid); else open_chat(visible_idx[list_sel]); } else { nav_mode = 1; list_sel_dirty = 1; } }
      if ((pressed & SCE_CTRL_SQUARE) && screen == S_LIST && nav_mode && nvis > 0 && list_sel < nvis && !popup_open && !menu_open) sel_toggle(chats[visible_idx[list_sel]].jid);   // Quadrato: seleziona la chat evidenziata
      if ((pressed & SCE_CTRL_CROSS) && (screen == S_UPDATES || screen == S_COMM || screen == S_CALLS) && !popup_open && !menu_open && !viewer_open) {
        if (tnav && tsel < nsr) { int sid = srows[tsel].id, sarg = srows[tsel].arg; if (sid == B_ROW) open_chat(sarg); else handle_extra(sid, sarg); } else { tnav = 1; tsel_dirty = 1; }
      }
      if (screen == S_STATUSVIEW && !popup_open && (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) st_step((pressed & (SCE_CTRL_RTRIGGER | SCE_CTRL_RIGHT)) ? 1 : -1);   // storie: R1/destra avanti, L1/sinistra indietro
      if (is_tab(screen) && !popup_open && !menu_open && !viewer_open && ((pressed & SCE_CTRL_LTRIGGER) || (pressed & SCE_CTRL_RTRIGGER))) {
        // tappe: 0 Tutte, 1 Non lette, 2 Gruppi, 3 Aggiornamenti, 4 Community, 5 Chiamate. R1 = avanti, L1 = indietro
        int cur = screen == S_LIST ? filter_sel : screen == S_UPDATES ? 3 : screen == S_COMM ? 4 : 5;
        int nx = cur + ((pressed & SCE_CTRL_RTRIGGER) ? 1 : -1);
        if (nx >= 0 && nx <= 5) { if (nx <= 2) { if (screen != S_LIST) goto_tab(0); filter_sel = nx; list_scroll = 0; list_sel = 0; list_sel_dirty = 1; } else goto_tab(nx - 2); }
      }
      if ((pressed & SCE_CTRL_TRIANGLE) && screen == S_CHAT && !ch_ro) emoji_open = !emoji_open;
      if ((pressed & SCE_CTRL_CROSS) && screen == S_CAMERA) cam_shoot();
      if ((pressed & SCE_CTRL_TRIANGLE) && screen == S_CAMERA) { int d = cam_dev ^ 1; cam_stop(); if (cam_start(d) < 0) cam_start(d ^ 1); }
      if ((pressed & SCE_CTRL_SQUARE) && screen == S_CHAT && !ch_ro) send_draft();
      if (screen == S_LIST && (pressed & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT)) running = 0;
    }

    if (ime_active && sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED) {
      SceImeDialogResult r; memset(&r, 0, sizeof r); sceImeDialogGetResult(&r);
      char t[2100]; u16to8(ime_in, t, sizeof t);              // il testo va letto PRIMA di chiudere la tastiera: dopo non e' piu' valido
      int ime_btn = r.button, ctx_done = ime_ctx;
      crumb("tastiera chiusa: pulsante=%d, testo di %u byte", ime_btn, (unsigned)strlen(t));
      sceImeDialogTerm(); ime_active = 0;
      if (ime_btn == SCE_IME_DIALOG_BUTTON_ENTER || ((ctx_done == IME_DRAFT || ctx_done == IME_CAPTION) && ime_btn == SCE_IME_DIALOG_BUTTON_CLOSE && t[0])) {   // chiudendo la tastiera la bozza resta nella barra
        if (ctx_done >= IME_SEARCH) handle_ime_extra(ctx_done, t);
        else if (ctx_done == IME_DRAFT) snprintf(draft, sizeof draft, "%s", t);
        else if (ctx_done == IME_CAPTION) snprintf(caption, sizeof caption, "%s", t);
        else if (ctx_done == IME_NAME && t[0]) { Job *j = job_new(J_PROFILE, NULL); if (j) { snprintf(j->kind, sizeof j->kind, "name"); snprintf(j->text, sizeof j->text, "%s", t); job_commit(); snprintf(prof.name, sizeof prof.name, "%s", t); } }
        else if (ctx_done == IME_ABOUT) { Job *j = job_new(J_PROFILE, NULL); if (j) { snprintf(j->kind, sizeof j->kind, "about"); snprintf(j->text, sizeof j->text, "%s", t); job_commit(); snprintf(prof.about, sizeof prof.about, "%s", t); } }
      }
    }
  }
  cam_stop(); crumb("uscita normale"); quit_flag = 1; pthread_join(th, NULL); if (engine_started) pthread_join(engine_th, NULL); sceIoRemove("ur0:tai/wa_active.txt"); sceIoRemove(LOCKFILE);
  upd_cleanup_helper();   // se resta l'app installatrice dell'ultimo aggiornamento, la si toglie ora (a grafica chiusa), non all'avvio
  sceKernelExitProcess(0);
  return 0;
}
