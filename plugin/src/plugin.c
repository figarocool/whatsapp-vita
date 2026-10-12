// Plugin taiHEN per la shell: interroga /events del bridge e mostra una notifica di sistema per ogni nuovo messaggio.
// Installazione: copia wa_notify.suprx in ur0:tai/ e aggiungi a ur0:tai/config.txt, sotto *main:  ur0:tai/wa_notify.suprx
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
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
void plog_s(const char *s) {   // riga di testo semplice nel registro
  char l[160]; int n = snprintf(l, sizeof l, "[%u] %s\n", (unsigned)(sceKernelGetProcessTimeLow() / 1000000), s);
  SceUID f = sceIoOpen(PLOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777); if (f >= 0) { sceIoWrite(f, l, n); sceIoClose(f); }
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



// ======================= client leggero WhatsApp dentro il plugin =======================
// L'app (motore) scrive ur0:tai/wa_lite.bin:  "WAL2" | chiave Noise privata(32) | pubblica(32) | u16 lunghezza | payload di login | u8 n.indirizzi | n x IPv4 | seme(32) | u8 lunghezza | numero dell'utente
// e ur0:tai/wa_names.txt:  una riga per chat  "jid<TAB>nome<TAB>flag"  (flag 1 = silenziata/archiviata) ; ur0:tai/wa_tpl.txt: due righe con i testi nella lingua dell'app.
#include "../lite/plat.h"
#include "../../engine/lite/lite.h"
#define LITEFILE "ur0:tai/wa_lite.bin"
#define NAMESFILE "ur0:tai/wa_names.txt"
#define TPLFILE "ur0:tai/wa_tpl.txt"
#define LASTNOTIF "ur0:tai/wa_lastnotif.txt"
#define HEAP_BYTES (640 * 1024)
void heap_init(void *base, size_t size); size_t heap_free_bytes(void);
int lv_net_resolve(void); int lv_net_init(void);

static SceUID heap_blk = -1;
static int heap_ready(void) {
  if (heap_blk >= 0) return 1; if (heap_blk == -2) return 0;
  heap_blk = sceKernelAllocMemBlock("wa_lite_heap", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (HEAP_BYTES + 0xFFF) & ~0xFFF, NULL);
  if (heap_blk < 0) { plog("memoria del client leggero non disponibile: 0x%x", heap_blk, 0); heap_blk = -2; return 0; }
  void *base = NULL; sceKernelGetMemBlockBase(heap_blk, &base); heap_init(base, HEAP_BYTES); plog("memoria del client leggero pronta (%d KB)", HEAP_BYTES / 1024, 0); return 1;
}
static uint8_t cred[2048]; static int cred_len; static uint8_t npriv[32], npub[32]; static const uint8_t *login_pl; static size_t login_pl_len; static char my_user[64];
static int load_creds(void) {
  SceUID f = sceIoOpen(LITEFILE, SCE_O_RDONLY, 0); if (f < 0) return 0;
  int n = sceIoRead(f, cred, sizeof cred); sceIoClose(f); if (n < 72 || memcmp(cred, "WAL2", 4)) return 0;
  memcpy(npriv, cred + 4, 32); memcpy(npub, cred + 36, 32); size_t pl = cred[68] | (cred[69] << 8); size_t o = 70; if (o + pl + 1 > (size_t)n) return 0;
  login_pl = cred + o; login_pl_len = pl; o += pl; int nip = cred[o++]; if (nip > 8 || o + nip * 4 + 33 > (size_t)n) return 0;
  uint32_t ipsv[8]; for (int i = 0; i < nip; i++) { memcpy(&ipsv[i], cred + o, 4); o += 4; }
  plat_set_ips(ipsv, nip); plat_set_seed(cred + o, 32); o += 32;
  int ml = o < (size_t)n ? cred[o++] : 0; if (ml > 62) ml = 62; if (o + ml > (size_t)n) ml = 0; memcpy(my_user, cred + o, ml); my_user[ml] = 0; cred_len = n; return nip > 0;
}
// file di testo piccolo -> buffer statico
static char names[24 * 1024]; static int names_len, names_loaded;
static void load_text(const char *path, char *buf, int cap, int *len) { *len = 0; SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0); if (f < 0) return; int n = sceIoRead(f, buf, cap - 1); sceIoClose(f); if (n > 0) { buf[n] = 0; *len = n; } }
static char tpl1[80] = "New message from %s", tpl2[96] = "New message from %s in %s";
static void load_tpl(void) {
  char b[256]; int n; load_text(TPLFILE, b, sizeof b, &n); if (n <= 0) return; char *nl = strchr(b, '\n');
  if (nl) { *nl = 0; char *l2 = nl + 1; char *e = strchr(l2, '\n'); if (e) *e = 0; if (strlen(l2) < sizeof tpl2 && strstr(l2, "%s")) strcpy(tpl2, l2); }
  if (strlen(b) < sizeof tpl1 && strstr(b, "%s")) strcpy(tpl1, b);
}
// cerca il nome del jid nel file dei nomi; ritorna 1 se trovato (e riempie nome/flag)
static int lookup_name(const char *jid, char *name, int cap, int *flag) {
  size_t jl = strlen(jid); if (!jl) return 0;
  for (const char *p = names; p < names + names_len;) {
    const char *e = strchr(p, '\n'); if (!e) e = names + names_len;
    if ((size_t)(e - p) > jl + 1 && !strncmp(p, jid, jl) && p[jl] == '\t') {
      const char *q = p + jl + 1; const char *t = q; while (t < e && *t != '\t') t++;
      int k = (int)(t - q); if (k > cap - 1) k = cap - 1; memcpy(name, q, k); name[k] = 0; *flag = (t < e && t[1] == '1') ? 1 : 0; return 1;
    }
    p = e + 1;
  }
  return 0;
}
static void bare_user(const char *jid, char *out, int cap) { int k = 0; while (jid[k] && jid[k] != '@' && jid[k] != ':' && k < cap - 1) { out[k] = jid[k]; k++; } out[k] = 0; }
static char seen_ids[48][56]; static int seen_n;
static int seen_before(const char *id) { if (!id[0]) return 0; for (int i = 0; i < 48; i++) if (!strcmp(seen_ids[i], id)) return 1; strcpy(seen_ids[seen_n++ % 48], id); return 0; }
static int numeric_name(const char *s) { if (!*s) return 1; for (; *s; s++) if ((*s < '0' || *s > '9') && *s != '+' && *s != ' ') return 0; return 1; }
static int burst; static unsigned burst_t0;
static void write_lastnotif(const char *jid) { char b[200]; int n = snprintf(b, sizeof b, "%s\t%u", jid, (unsigned)(sceKernelGetSystemTimeWide() / 1000)); SceUID f = sceIoOpen(LASTNOTIF, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (f >= 0) { sceIoWrite(f, b, n); sceIoClose(f); } }
static void on_node(const LiteNode *n, void *ud) {
  (void)ud; if (strcmp(n->tag, "message")) return;
  if (!strcmp(n->category, "peer") || !strcmp(n->type, "reaction")) return;
  char who[48]; const char *sender = n->participant[0] ? n->participant : n->from; bare_user(sender, who, sizeof who);
  { const char *q = my_user; while (*q) { const char *e = q; while (*e && *e != ',') e++; if ((size_t)(e - q) == strlen(who) && !strncmp(q, who, e - q)) return; q = *e ? e + 1 : e; } }   // messaggi scritti da un altro mio dispositivo (numero o lid)
  { char fu[48]; bare_user(n->from, fu, sizeof fu); const char *q = my_user; while (*q && n->recipient[0]) { const char *e = q; while (*e && *e != ',') e++; if ((size_t)(e - q) == strlen(fu) && !strncmp(q, fu, e - q)) return; q = *e ? e + 1 : e; } }   // da me verso un altro (c'e' il destinatario)
  if (seen_before(n->id) || notif_off()) return;
  char nm[64] = "", gnm[64] = ""; int flag = 0, flag2 = 0; int isg = strstr(n->from, "@g.us") != NULL;
  if (!names_loaded) { load_text(NAMESFILE, names, sizeof names, &names_len); load_tpl(); names_loaded = 1; }
  int have = lookup_name(n->from, isg ? gnm : nm, 64, &flag);
  if (have && flag) return;   // chat silenziata o archiviata
  if (isg) { if (!lookup_name(n->participant, nm, 64, &flag2)) { if (n->notify[0]) snprintf(nm, sizeof nm, "%s", n->notify); else snprintf(nm, sizeof nm, "%s", who); } if (!gnm[0]) snprintf(gnm, sizeof gnm, "?"); }
  else if (!nm[0]) { if (n->notify[0]) snprintf(nm, sizeof nm, "%s", n->notify); else snprintf(nm, sizeof nm, "%s", who); }
  if (!isg && n->notify[0] && numeric_name(nm)) snprintf(nm, sizeof nm, "%s", n->notify);   // contatto senza nome in rubrica: vale il nome scelto dall'altro
  unsigned now = sceKernelGetProcessTimeLow() / 1000000; if (now - burst_t0 > 20) { burst_t0 = now; burst = 0; }
  if (++burst > 4) return;   // ondata di messaggi arretrati: solo i primi
  char line[160]; if (isg) snprintf(line, sizeof line, tpl2, nm, gnm); else snprintf(line, sizeof line, tpl1, nm);
  write_lastnotif(n->from); plog("notifica: messaggio in arrivo (%d)", burst, 0);
  if (notice_send(line) != 0) send_notif(line);
}

static void touch_alive(void) { SceUID f = sceIoOpen(ALIVE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (f >= 0) { sceIoWrite(f, "1", 1); sceIoClose(f); } }
// Salvaguardia: se il client leggero fa crashare la shell, al riavvio successivo il tentativo e' registrato; dopo 2 crash di fila il plugin non lo prova piu' (cancellare wa_try.txt per riprovare).
#define TRYFILE "ur0:tai/wa_try.txt"
static int try_count(void) { char b[4] = ""; SceUID f = sceIoOpen(TRYFILE, SCE_O_RDONLY, 0); if (f < 0) return 0; int n = sceIoRead(f, b, 3); sceIoClose(f); return n > 0 ? b[0] - '0' : 0; }
static int try_armed;
static void try_arm(void) { if (try_armed) return; try_armed = 1; int n = try_count() + 1; char b[2] = { (char)('0' + (n > 9 ? 9 : n)), 0 }; SceUID f = sceIoOpen(TRYFILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (f >= 0) { sceIoWrite(f, b, 1); sceIoClose(f); } }
static void try_clear(void) { if (!try_armed) return; try_armed = 0; sceIoRemove(TRYFILE); }
static LiteClient *client; static unsigned connected_at; static unsigned backoff_s = 20, next_try; static int net_inited;
static void drop_client(void) { if (client) { lite_close(client); client = NULL; } try_clear(); }
static int worker(SceSize args, void *argp) {
  (void)args; (void)argp;
  plog("plugin avviato (thread attivo)", 0, 0);
  if (try_count() >= 2) { plog("client leggero disattivato: i due avvii precedenti non sono finiti bene (cancella ur0:tai/wa_try.txt per riprovare)", 0, 0); for (;;) { touch_alive(); sceKernelDelayThread(10 * 1000 * 1000); } }
  sceKernelDelayThread(15 * 1000 * 1000);        // attende che la shell e il Wi-Fi siano pronti
  int loops = 0, was_active = 1;
  for (;;) {
    if (loops++ % 4 == 0) touch_alive();
    { SceIoStat ts; if (sceIoGetstat(NOTIF_TEST, &ts) >= 0) { sceIoRemove(NOTIF_TEST); plog("richiesta di notifica di prova", 0, 0); if (notice_send("WhatsApp: prova di notifica riuscita") != 0) send_notif("WhatsApp: prova di notifica riuscita"); } }
    int active_now = app_active();
    if (active_now || notif_off()) { if (client) { plog("app aperta: collegamento del plugin chiuso", 0, 0); drop_client(); } was_active = 1; sceKernelDelayThread(3 * 1000 * 1000); continue; }
    unsigned now = sceKernelGetProcessTimeLow() / 1000000;
    if (was_active) { was_active = 0; next_try = now + 8; names_loaded = 0; }   // l'app si e' appena chiusa: le si lascia il tempo di salvare, poi si collega il plugin
    if (!client && now >= next_try) {
      if (!load_creds() || !heap_ready()) { next_try = now + 30; }
      else {
        if (!net_inited) { if (lv_net_resolve()) { int r = lv_net_init(); plog("rete pronta (0x%x)", r, 0); net_inited = 1; } else { next_try = now + 15; } }
        if (net_inited) {
          try_arm(); client = lite_new(npriv, npub, login_pl, login_pl_len, NULL);
          int rc = client ? lite_connect(client) : -99;
          if (rc == 0) { plog("client leggero collegato (memoria libera %d KB)", (int)(heap_free_bytes() / 1024), 0); backoff_s = 20; connected_at = now; }
          else { plog("collegamento non riuscito: %d", rc, 0); if (client) { plog_s(lite_last_error(client)); } drop_client(); try_clear(); next_try = now + backoff_s; if (backoff_s < 300) backoff_s *= 2; }
        }
      }
    }
    if (client) {
      if (try_armed && now - connected_at > 40) try_clear();   // 40 secondi di collegamento stabile: nessun crash
      int rc = lite_poll(client, 1500, on_node, NULL);
      if (rc != 0) {
        plog("collegamento interrotto: %d", rc, 0); if (rc == LITE_CONFLICT) { next_try = now + 120; plog("sostituito da un altro dispositivo (l'app?): attendo", 0, 0); } else { next_try = now + 15; }
        drop_client();
      }
    } else sceKernelDelayThread(2 * 1000 * 1000);
  }
  return 0;
}

int module_start(SceSize argc, const void *args) {
  (void)argc; (void)args;
  SceUID t = sceKernelCreateThread("wa_notify", worker, 0x10000100, 0x40000, 0, 0, NULL);
  if (t >= 0) sceKernelStartThread(t, 0, NULL);
  return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize argc, const void *args) { (void)argc; (void)args; return SCE_KERNEL_STOP_SUCCESS; }
