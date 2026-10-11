// Plugin taiHEN per la shell: interroga /events del bridge e mostra una notifica di sistema per ogni nuovo messaggio.
// Installazione: copia wa_notify.suprx in ur0:tai/ e aggiungi a ur0:tai/config.txt, sotto *main:  ur0:tai/wa_notify.suprx
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/modulemgr.h>
#include <taihen.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define CFG "ur0:tai/wa_config.txt"
#define ACTIVE "ur0:tai/wa_active.txt"
#define ALIVE "ur0:tai/wa_alive.txt"       // il plugin lo aggiorna ogni ~15 s: l'app lo legge per sapere se e' attivo
#define NOTIF_TEST "ur0:tai/wa_notif_test.txt"    // l'app lo crea per chiedere una notifica di prova
#define NOTIF_OFF "ur0:tai/wa_notif_off.txt"   // se esiste, le notifiche sono disattivate dalle impostazioni dell'app   // l'app lo aggiorna ogni pochi secondi mentre e' aperta

#define PLOG "ur0:tai/wa_plugin.log"
static void plog(const char *fmt, int a, int b) {   // registro di diagnostica del plugin (solo quando lo stato cambia)
  char m[160], l[200]; snprintf(m, sizeof m, fmt, a, b);
  int n = snprintf(l, sizeof l, "[%u] %s\n", (unsigned)(sceKernelGetProcessTimeLow() / 1000000), m);
  SceUID f = sceIoOpen(PLOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777); if (f >= 0) { sceIoWrite(f, l, n); sceIoClose(f); }
}
static char host_ip[20] = "192.168.1.10"; static int host_port = 8787;

static void load_cfg(void) {
  SceUID f = sceIoOpen(CFG, SCE_O_RDONLY, 0); if (f < 0) return;
  char b[200] = ""; int n = sceIoRead(f, b, sizeof b - 1); sceIoClose(f); if (n <= 0) return; b[n] = 0;
  char *p = strstr(b, "://"); p = p ? p + 3 : b;
  char ip[20]; int port = 80, k = 0;
  while (*p && *p != ':' && *p != '/' && *p != '\n' && *p != '\r' && k < 19) ip[k++] = *p++;
  ip[k] = 0; if (*p == ':') port = atoi(p + 1);
  if (k) { snprintf(host_ip, sizeof host_ip, "%s", ip); host_port = port; }
}

static int notif_off(void) { SceIoStat st; return sceIoGetstat(NOTIF_OFF, &st) >= 0; }
// L'app tocca il file "active" ogni pochi secondi. Senza orologio: si guarda se la sua data di modifica cambia (nell'ultimo minuto = app aperta).
static int app_active(void) {
  static SceDateTime last_m; static unsigned last_change; static int seen;
  SceIoStat st; if (sceIoGetstat(ACTIVE, &st) < 0) { seen = 0; return 0; }
  unsigned now = sceKernelGetProcessTimeLow() / 1000000;
  if (!seen) { last_m = st.st_mtime; last_change = now - 1000; seen = 1; }   // primo sguardo: non sappiamo da quanto e' fermo, quindi app chiusa
  else if (memcmp(&last_m, &st.st_mtime, sizeof last_m) != 0) { last_m = st.st_mtime; last_change = now; }
  return now - last_change < 25;
}

// Le funzioni di rete NON sono collegate in modo fisso: la shell rifiuta i plugin che importano librerie non ancora caricate.
// Si cercano a runtime tra le esportazioni del modulo SceNetPs (identificativi presi dagli stub del SDK).
typedef int (*f_init)(SceNetInitParam *); typedef int (*f_socket)(const char *, int, int, int);
typedef int (*f_setopt)(int, int, int, const void *, unsigned); typedef int (*f_connect)(int, const SceNetSockaddr *, unsigned);
typedef int (*f_send)(int, const void *, unsigned, int); typedef int (*f_recv)(int, void *, unsigned, int); typedef int (*f_close)(int);
static f_init n_init; static f_socket n_socket; static f_setopt n_setopt; static f_connect n_connect; static f_send n_send; static f_recv n_recv; static f_close n_close;
static int net_ready;
// cerca una funzione in tutti i moduli caricati il cui nome inizia per "SceNet" (il nome esatto del modulo varia da firmware a firmware)
static uintptr_t find_net_fn(unsigned nid) {
  static SceUID ids[256]; static SceKernelModuleInfo mi; SceSize cnt = 256;
  if (sceKernelGetModuleList(0xFF, ids, &cnt) < 0) return 0;
  for (SceSize i = 0; i < cnt; i++) {
    mi.size = sizeof mi; if (sceKernelGetModuleInfo(ids[i], &mi) < 0) continue;
    if (strncmp(mi.module_name, "SceNet", 6) != 0) continue;
    uintptr_t fn = 0; if (taiGetModuleExportFunc(mi.module_name, TAI_ANY_LIBRARY, nid, &fn) >= 0 && fn) return fn;
  }
  return 0;
}
static void plog_s(const char *s) {   // riga di testo semplice nel registro
  char l[120]; int n = snprintf(l, sizeof l, "[%u] %s\n", (unsigned)(sceKernelGetProcessTimeLow() / 1000000), s);
  SceUID f = sceIoOpen(PLOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777); if (f >= 0) { sceIoWrite(f, l, n); sceIoClose(f); }
}
static void log_net_modules(void) {
  static SceUID ids[256]; static SceKernelModuleInfo mi; SceSize cnt = 256;
  if (sceKernelGetModuleList(0xFF, ids, &cnt) < 0) { plog("elenco moduli non leggibile", 0, 0); return; }
  plog("moduli caricati: %d", (int)cnt, 0);
  for (SceSize i = 0; i < cnt; i++) {
    mi.size = sizeof mi; if (sceKernelGetModuleInfo(ids[i], &mi) < 0) continue;
    if (strncmp(mi.module_name, "SceNet", 6) == 0 || strncmp(mi.module_name, "SceHttp", 7) == 0 || strncmp(mi.module_name, "SceSsl", 6) == 0) plog_s(mi.module_name);
  }
}
static int net_resolve(void) {
  if (net_ready) return 1;
  static int loaded; if (!loaded) { loaded = 1; int r = sceSysmoduleLoadModule(SCE_SYSMODULE_NET); plog("caricamento modulo rete -> 0x%x", r, 0); }
  static const unsigned nids[7] = { 0xEB03E265u, 0xF084FCE3u, 0x065505CAu, 0x11E5B6F6u, 0xE3DD8CD9u, 0x023643B7u, 0x29822B4Du };
  uintptr_t *dst[7] = { (uintptr_t *)&n_init, (uintptr_t *)&n_socket, (uintptr_t *)&n_setopt, (uintptr_t *)&n_connect, (uintptr_t *)&n_send, (uintptr_t *)&n_recv, (uintptr_t *)&n_close };
  for (int i = 0; i < 7; i++) {
    uintptr_t fn = find_net_fn(nids[i]);
    if (!fn) { static int last = -1; if (last != i) { plog("funzione di rete %d non trovata in nessun modulo SceNet*", i, 0); log_net_modules(); last = i; } return 0; }
    *dst[i] = fn;
  }
  net_ready = 1; plog("funzioni di rete trovate", 0, 0); return 1;
}
// "a.b.c.d" -> indirizzo di rete (senza SceLibc)
static int parse_ip(const char *s, unsigned *out) {
  unsigned b[4]; int k = 0;
  while (k < 4) { if (*s < '0' || *s > '9') return 0; unsigned v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); if (v > 255) return 0; } b[k++] = v; if (k < 4) { if (*s != '.') return 0; s++; } }
  *out = b[0] | (b[1] << 8) | (b[2] << 16) | ((unsigned)b[3] << 24); return 1;
}

// Una transazione col canale del bridge (porta+1): si invia una riga e si leggono AL MASSIMO 16 byte.
// Il limite e' voluto: nella shell, leggere piu' di ~256 byte in una transazione rompe le notifiche fino al riavvio.
#define RX_MAX 16
static int raw_tx(const char *req, char *out) {
  if (!net_resolve()) return -1;
  int s = n_socket("wa", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0); if (s < 0) { static int last = 0; if (s != last) { plog("sceNetSocket errore 0x%x", s, 0); last = s; } return -1; }
  int to = 6 * 1000 * 1000; n_setopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &to, sizeof to); n_setopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &to, sizeof to);
  SceNetSockaddrIn a; memset(&a, 0, sizeof a); a.sin_len = sizeof a; a.sin_family = SCE_NET_AF_INET;
  int port = host_port + 1; a.sin_port = (unsigned short)(((port & 0xFF) << 8) | ((port >> 8) & 0xFF));
  { unsigned ipn; if (!parse_ip(host_ip, &ipn)) { n_close(s); return -1; } a.sin_addr.s_addr = ipn; }
  { int cr = n_connect(s, (SceNetSockaddr *)&a, sizeof a); if (cr < 0) { static int last = 0; if (cr != last) { plog("connessione al bridge fallita 0x%x", cr, 0); last = cr; } n_close(s); return -1; } }
  if (n_send(s, req, strlen(req), 0) < 0) { n_close(s); return -1; }
  int n = 0, r; while (n < RX_MAX && (r = n_recv(s, out + n, RX_MAX - n, 0)) > 0) n += r;
  n_close(s); out[n] = 0; return n;
}

static void utf8to16(const char *s, SceWChar16 *o, int max) {
  int k = 0;
  while (*s && k < max - 2) {
    unsigned char c = *s; unsigned cp; int l;
    if (c < 0x80) { cp = c; l = 1; } else if ((c >> 5) == 6) { cp = c & 31; l = 2; } else if ((c >> 4) == 14) { cp = c & 15; l = 3; } else { cp = c & 7; l = 4; }
    for (int i = 1; i < l && s[i]; i++) cp = (cp << 6) | (s[i] & 63);
    s += l;
    if (cp >= 0x10000) { cp -= 0x10000; o[k++] = 0xD800 + (cp >> 10); o[k++] = 0xDC00 + (cp & 0x3FF); } else o[k++] = cp;
  }
  o[k] = 0;
}

// La libreria delle notifiche NON e' collegata in modo fisso (se nella shell non e' caricata la Vita rifiuterebbe tutto il plugin):
// la si carica qui, a runtime, e si cerca la funzione tra le esportazioni del modulo.
typedef int (*notif_fn)(const SceWChar16 *);
static notif_fn p_send; static int notif_tried;
typedef int (*bg_fn)(void); static bg_fn p_bginit; static int bg_done;
static void notif_init(void) {
  notif_tried = 1;
  int r = sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL); plog("caricamento SceNotificationUtil -> 0x%x", r, 0);
  uintptr_t fn = 0, bg = 0;
  int e = taiGetModuleExportFunc("SceNotificationUtil", TAI_ANY_LIBRARY, 0xDE6F33F4u, &fn); plog("ricerca SendNotification -> 0x%x", e, 0); if (e < 0) fn = 0;
  p_send = (notif_fn)fn; p_bginit = (bg_fn)bg;
}
// Un plugin della shell e' una "app in background": prima di mandare notifiche la libreria va inizializzata con BgAppInitialize.
static void bg_init_once(void) { if (bg_done || !p_bginit) return; bg_done = 1; int r = p_bginit(); plog("BgAppInitialize -> 0x%x", r, 0); }
static int send_notif(const char *utf8) {
  static SceWChar16 w[0x410 / 2]; memset(w, 0, sizeof w); utf8to16(utf8, w, 250);
  if (!notif_tried) notif_init();
  if (!p_send) { plog("notifica non inviata: funzione assente", 0, 0); return -1; }
  int r = p_send(w); plog("notifica inviata, esito 0x%x", r, 0); return r;
}

// --- notifica "vera" della shell, con azione al tocco (stesso meccanismo di VitaDB Downloader / SceShell-Notice-PoC) ---
// Si usano tre funzioni INTERNE della shell, trovate per offset: valgono solo per i firmware elencati sotto. Per tutti gli altri
// il plugin non tocca niente (e scrive nel registro l'identificativo della shell, cosi' si puo' aggiungere il firmware).
#define APP_TITLEID "WHAP00001"
typedef int (*sh_init_fn)(void *); typedef int (*sh_utf8_fn)(void *, const char *, SceSize); typedef int (*lsdb_fn)(void *, int);
static sh_init_fn sh_init, sh_clean; static sh_utf8_fn sh_utf8; static lsdb_fn p_lsdb; static int notice_state;   // 0 da provare, 1 pronto, -1 non disponibile
static int seg_off(SceUID modid, unsigned off, void *out) {
  SceKernelModuleInfo mi; mi.size = sizeof mi; int r = sceKernelGetModuleInfo(modid, &mi); if (r < 0) return r;
  if (off > mi.segments[0].memsz) return -3; *(unsigned *)out = (unsigned)mi.segments[0].vaddr + off; return 0;
}
static void notice_init(void) {
  notice_state = -1;
  tai_module_info_t info; info.size = sizeof info; int r = taiGetModuleInfo("SceShell", &info);
  plog("SceShell: ricerca 0x%x, identificativo modulo 0x%x", r, (int)info.module_nid); if (r < 0) return;
  unsigned o_init, o_utf8, o_clean;
  switch (info.module_nid) {
    case 0x0552F692: o_init = 0x42930C; o_utf8 = 0x408E14; o_clean = 0x4163E8; break;   // 3.60 retail
    case 0x5549BF1F: o_init = 0x429754; o_utf8 = 0x40925C; o_clean = 0x416830; break;   // 3.65 retail
    default: plog("firmware della shell non supportato per le notifiche con tocco", 0, 0); return;
  }
  if (seg_off(info.modid, o_init | 1, &sh_init) < 0 || seg_off(info.modid, o_utf8 | 1, &sh_utf8) < 0 || seg_off(info.modid, o_clean | 1, &sh_clean) < 0) { plog("offset della shell non validi", 0, 0); return; }
  uintptr_t fn = 0; int e = taiGetModuleExportFunc("SceLsdb", TAI_ANY_LIBRARY, 0x315B9FD6u, &fn); plog("ricerca sceLsdbSendNotification -> 0x%x", e, 0);
  if (e < 0 || !fn) return;
  p_lsdb = (lsdb_fn)fn; notice_state = 1; plog("notifiche con tocco pronte", 0, 0);
}
static int notice_send(const char *text) {
  if (!notice_state) notice_init();
  if (notice_state <= 0) return -1;
  static char data[0x100] __attribute__((aligned(8))); static unsigned n; char fname[64], item[16];
  snprintf(fname, sizeof fname, "ux0:app/%s/sce_sys/icon0.png", APP_TITLEID); snprintf(item, sizeof item, "WA%u", ++n);   // id diverso per ogni messaggio: non si sostituiscono
  sh_init(data);
  sh_utf8(&data[0x0], APP_TITLEID, strlen(APP_TITLEID));
  sh_utf8(&data[0xC], item, strlen(item));
  *(unsigned *)&data[0x28] = 0x02;              // al tocco: apre l'app
  data[0x2C] = 0x01;
  sh_utf8(&data[0x30], fname, strlen(fname));   // icona
  sh_utf8(&data[0xBC], text, strlen(text));     // testo
  *(unsigned *)&data[0xC8] = 0x20000;
  sh_utf8(&data[0xCC], APP_TITLEID, strlen(APP_TITLEID));   // app da aprire
  sh_utf8(&data[0xD8], "chat", 4);                          // argomento di avvio
  int r = p_lsdb(data, 1); sh_clean(data); plog("notifica con tocco inviata, esito 0x%x", r, 0); return r;
}

static void touch_alive(void) { SceUID f = sceIoOpen(ALIVE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (f >= 0) { sceIoWrite(f, "1", 1); sceIoClose(f); } }
static int worker(SceSize args, void *argp) {
  (void)args; (void)argp;
  plog("plugin avviato (thread attivo)", 0, 0);
  long since = -1;
  sceKernelDelayThread(15 * 1000 * 1000);        // attende che la shell e il Wi-Fi siano pronti
  if (net_resolve()) { static char netpool[64 * 1024]; SceNetInitParam ip = { netpool, sizeof netpool, 0 }; int r = n_init(&ip); plog("sceNetInit -> 0x%x (errore 'gia' inizializzato' e' normale)", r, 0); }
  int loops = 0;
  for (;;) {
    if (loops++ % 4 == 0) touch_alive();
    { SceIoStat ts; if (sceIoGetstat(NOTIF_TEST, &ts) >= 0) { sceIoRemove(NOTIF_TEST); plog("richiesta di notifica di prova", 0, 0); if (notice_send("WhatsApp: prova di notifica riuscita") != 0) send_notif("WhatsApp: prova di notifica riuscita"); } }
    { SceIoStat ts2; if (sceIoGetstat("ur0:tai/wa_notif_test3.txt", &ts2) >= 0) { sceIoRemove("ur0:tai/wa_notif_test3.txt"); plog("prova notifica con tocco (shell)", 0, 0); notice_send("WhatsApp: tocca per aprire"); } }
    load_cfg();
    int active_now = app_active();   // va chiamata a ogni giro: serve a vedere il file cambiare
    char req[48], rx[RX_MAX + 1];
    snprintf(req, sizeof req, "P %ld\n", since < 0 ? 0L : since);
    int n = raw_tx(req, rx);
    if (n > 1 && rx[0] == 'S') {
      long seq = atoi(rx + 1); const char *c = strstr(rx, ","); int cnt = c ? atoi(c + 1) : 0;
      { static int first = 1; if (first) { plog("primo contatto col bridge riuscito, seq=%d", (int)seq, 0); first = 0; } }
      if (since < 0 || seq < since) { since = seq; cnt = 0; }   // primo giro, o bridge riavviato: nessuna notifica retroattiva
      if (cnt > 0 && !active_now && !notif_off()) {
        for (int i = 0; i < cnt && i < 5; i++) {
          char line[100]; int len = 0, off = 0;
          for (;;) {
            snprintf(req, sizeof req, "G %ld %d %d\n", since, i, off);
            int m = raw_tx(req, rx); if (m < 0) { len = -1; break; }
            if (len + m < (int)sizeof line - 1) { memcpy(line + len, rx, m); len += m; }
            off += m; if (m < RX_MAX || len >= (int)sizeof line - 17) break;
          }
          if (len > 0) { line[len] = 0; if (notice_send(line) != 0) send_notif(line); sceKernelDelayThread(1500 * 1000); }   // prima la notifica con tocco; se non c'e' (firmware) o fallisce, il popup semplice
        }
      }
      since = seq;
    }
    sceKernelDelayThread(4 * 1000 * 1000);
  }
  return 0;
}

int module_start(SceSize argc, const void *args) {
  (void)argc; (void)args;
  SceUID t = sceKernelCreateThread("wa_notify", worker, 0x10000100, 0x10000, 0, 0, NULL);
  if (t >= 0) sceKernelStartThread(t, 0, NULL);
  return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize argc, const void *args) { (void)argc; (void)args; return SCE_KERNEL_STOP_SUCCESS; }
