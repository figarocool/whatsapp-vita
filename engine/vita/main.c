// App di prova del motore WhatsApp sulla Vita: avvia QuickJS + Baileys in un thread e mostra il log a schermo.
#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "engine.h"

int _newlib_heap_size_user = 190 * 1024 * 1024;

#define NL 28
static char lines[NL][118]; static volatile unsigned nlines;
static SceUID logf_ = -1; static unsigned long long t_start;
static void on_log(const char *l) {
  unsigned long long el = (sceKernelGetProcessTimeWide() - t_start) / 1000;
  char b[400]; snprintf(b, sizeof b, "[%llu.%03llu] %s", el / 1000, el % 1000, l);
  sceClibPrintf("WAENG %s\n", b);
  { SceUID f = sceIoOpen("ux0:data/waengine/log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777); if (f >= 0) { sceIoWrite(f, b, strlen(b)); sceIoWrite(f, "\n", 1); sceIoClose(f); } }
  if (!strncmp(l, "QR:", 3)) snprintf(b, sizeof b, "[%llu.%03llu] QR ricevuto (%d caratteri)", el / 1000, el % 1000, (int)strlen(l) - 3);
  const char *p = b; while (*p) {
    char *dst = lines[nlines % NL]; size_t n = strlen(p) > 116 ? 116 : strlen(p); memcpy(dst, p, n); dst[n] = 0; nlines++; p += n;
  }
}
static volatile int quit_flag, eng_done;
static int engine_thread(SceSize args, void *argp) {
  Engine *e = eng_new("app0:assets/cacert.pem", "ux0:data/waengine", on_log);
  if (eng_run_module(e, "app0:assets/engine.bundle.js") == 0) while (!quit_flag && eng_loop_step(e, 20) > 0) {}
  on_log("motore fermo"); eng_done = 1; return 0;
}
int main(void) {
  t_start = sceKernelGetProcessTimeWide();
  sceIoMkdir("ux0:data", 0777); sceIoMkdir("ux0:data/waengine", 0777); sceIoMkdir("ux0:data/waengine/auth", 0777);
  sceIoRemove("ux0:data/waengine/log.txt");
  sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  static char netmem[256 * 1024]; SceNetInitParam ip = { netmem, sizeof netmem, 0 }; sceNetInit(&ip); sceNetCtlInit();
  vita2d_init(); vita2d_set_clear_color(RGBA8(0, 0, 0, 255)); vita2d_pgf *pgf = vita2d_load_default_pgf();
  on_log("avvio del motore...");
  SceUID th = sceKernelCreateThread("engine", engine_thread, 0x40, 4 * 1024 * 1024, 0, 0, NULL); sceKernelStartThread(th, 0, NULL);
  SceCtrlData pad; unsigned old = 0;
  for (;;) {
    sceCtrlPeekBufferPositive(0, &pad, 1); unsigned pr = pad.buttons & ~old; old = pad.buttons; if (pr & SCE_CTRL_START) break;
    vita2d_start_drawing(); vita2d_clear_screen();
    unsigned first = nlines > NL ? nlines - NL : 0;
    for (unsigned i = first; i < nlines; i++) vita2d_pgf_draw_text(pgf, 10, 22 + (i - first) * 19, RGBA8(220, 255, 220, 255), 0.8f, lines[i % NL]);
    char st[120]; snprintf(st, sizeof st, "Motore WhatsApp (prova) - START per uscire - memoria libera heap ok");
    vita2d_pgf_draw_text(pgf, 10, 540, RGBA8(255, 255, 0, 255), 0.75f, st);
    vita2d_end_drawing(); vita2d_swap_buffers();
  }
  quit_flag = 1; sceKernelDelayThread(500000);
  if (logf_ >= 0) sceIoClose(logf_);
  sceKernelExitProcess(0); return 0;
}
