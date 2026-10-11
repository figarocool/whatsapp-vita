// Plugin di PROVA a livelli, per capire dove si ferma il caricamento nella shell della Vita.
// STAGE 0: solo module_start (scrive un file).  1: + thread.  2: + caricamento libreria notifiche e ricerca funzione (taiHEN).  3: + rete.
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <taihen.h>
#include <stdio.h>
#include <string.h>

#ifndef STAGE
#define STAGE 0
#endif
#define S2(x) #x
#define S1(x) S2(x)
#define LOGF "ux0:data/whatsapp/probe_s" S1(STAGE) ".log"

// il registro va su ur0: (sempre pronta, anche se il plugin parte prima che ux0: sia montata) e, quando c'e', anche su ux0:
#define LOGU "ur0:tai/probe_s" S1(STAGE) ".log"
static void plog_to(const char *path, const char *l, int n) {
  SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777); if (f >= 0) { sceIoWrite(f, l, n); sceIoClose(f); }
}
static void plog(const char *msg, int a) {
  char l[160]; int n = snprintf(l, sizeof l, "[%u] %s %d\n", (unsigned)(sceKernelGetProcessTimeLow() / 1000000), msg, a);
  plog_to(LOGU, l, n); plog_to(LOGF, l, n);
}

#if STAGE >= 1
static int worker(SceSize args, void *argp) {
  (void)args; (void)argp;
  plog("thread attivo", 1);
  { int w = 0; SceUID d; while ((d = sceIoDopen("ux0:data")) < 0 && w < 120) { sceKernelDelayThread(1000 * 1000); w++; } if (d >= 0) sceIoDclose(d); plog("ux0: pronta dopo (secondi)", w); }
#if STAGE >= 2
  { int r = sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL); plog("sceSysmoduleLoadModule(NOTIFICATION_UTIL) ->", r);
    uintptr_t fn = 0; int e = taiGetModuleExportFunc("SceNotificationUtil", TAI_ANY_LIBRARY, 0xDE6F33F4u, &fn); plog("taiGetModuleExportFunc(SendNotification) ->", e); plog("indirizzo funzione:", (int)fn); }
#endif
#if STAGE >= 3
  { static char pool[64 * 1024]; SceNetInitParam ip = { pool, sizeof pool, 0 }; int r = sceNetInit(&ip); plog("sceNetInit ->", r);
    int s = sceNetSocket("probe", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0); plog("sceNetSocket ->", s); if (s >= 0) sceNetSocketClose(s); }
#endif
  for (int i = 0; i < 4; i++) { sceKernelDelayThread(10 * 1000 * 1000); plog("ancora vivo, giro", i); }
  return 0;
}
#endif

int module_start(SceSize argc, const void *args) {
  (void)argc; (void)args;
  plog("module_start eseguito, fase", STAGE);
#if STAGE >= 1
  SceUID t = sceKernelCreateThread("wa_probe", worker, 0x10000100, 0x10000, 0, 0, NULL);
  plog("sceKernelCreateThread ->", t); if (t >= 0) { int r = sceKernelStartThread(t, 0, NULL); plog("sceKernelStartThread ->", r); }
#endif
  return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize argc, const void *args) { (void)argc; (void)args; return SCE_KERNEL_STOP_SUCCESS; }
