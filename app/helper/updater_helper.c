// Installatore di WhatsApp Vita (app WHAPUPD01). Non puo' farlo l'app stessa: un'app non puo' sostituirsi mentre gira.
// Chiude l'app, installa il pacchetto gia' estratto in ux0:/data/whatsapp/updates/core-install e riapre WhatsApp.
// Schema ripreso da Desktop Mode (core_updater.c); la preparazione del pacchetto (head.bin) viene da VitaShell, GPL-3.0.
#include <psp2/appmgr.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>
#include "../src/fpkg_head.h"
#include <stdio.h>
#include <string.h>

#define TITLE_ID "WHAP00001"
#define INSTALL_ROOT "ux0:/data/whatsapp/updates/core-install"
#define VERSION_FILE INSTALL_ROOT "/assets/version.txt"
#define FAILURE_FILE "ux0:/data/whatsapp/updates/.install-failed"
#define VERSION_MARKER "ux0:/data/whatsapp/updates/core-install.tag"

static int load_paf(void) {
  uint32_t args[6] = { 0x180000u, UINT32_MAX, UINT32_MAX, 1u, UINT32_MAX, UINT32_MAX }; int result = -1;
  uint32_t option[4] = { (uint32_t)sizeof(option), (uint32_t)&result, UINT32_MAX, UINT32_MAX };
  return sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(args), args, (SceSysmoduleOpt *)option);
}
static int contains_title(const char *path, const char *id) {
  unsigned char data[8192]; int fd = sceIoOpen(path, SCE_O_RDONLY, 0); if (fd < 0) return 0;
  int n = sceIoRead(fd, data, sizeof(data)); sceIoClose(fd); size_t len = strlen(id);
  for (int i = 0; i < n - (int)len + 1; i++) if (!memcmp(data + i, id, len)) return 1; return 0;
}
static int read_line(const char *path, char *out, size_t cap) {
  int fd = sceIoOpen(path, SCE_O_RDONLY, 0); if (fd < 0) return 0; int n = sceIoRead(fd, out, cap - 1); sceIoClose(fd); if (n <= 0) return 0;
  out[n] = 0; out[strcspn(out, "\r\n")] = 0; return 1;
}
static int same_version(void) {
  char expected[64] = { 0 }, actual[64] = { 0 };
  return read_line(VERSION_MARKER, expected, sizeof expected) && read_line(VERSION_FILE, actual, sizeof actual) && expected[0] && !strcmp(expected, actual);
}
static void report_failure(int error) {
  char tag[64] = { 0 }, line[96]; read_line(VERSION_MARKER, tag, sizeof tag);
  int fd = sceIoOpen(FAILURE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd >= 0) { int n = snprintf(line, sizeof line, "%s %08X\n", tag, (unsigned)error); if (n > 0 && (size_t)n < sizeof line) sceIoWrite(fd, line, (size_t)n); sceIoClose(fd); }
}
static int promote_app(void) {
  char param[128], eboot[128]; SceIoStat st;
  snprintf(param, sizeof param, "%s/sce_sys/param.sfo", INSTALL_ROOT); snprintf(eboot, sizeof eboot, "%s/eboot.bin", INSTALL_ROOT);
  if (!same_version() || !contains_title(param, TITLE_ID) || sceIoGetstat(eboot, &st) < 0 || st.st_size < 1024) return -1;
  if (wa_make_head_bin(INSTALL_ROOT, "app0:/assets/vitashell-head.bin", TITLE_ID) < 0) return -2;
  int res = load_paf(); if (res < 0) return res;
  res = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
  if (res < 0) { sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, NULL); return res; }
  res = scePromoterUtilityInit();
  if (res >= 0) { res = scePromoterUtilityPromotePkgWithRif(INSTALL_ROOT, 1); scePromoterUtilityExit(); }
  sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
  sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, NULL);
  return res;
}
// Schermata fissa "Aggiornamento in corso" (immagine gia' pronta, scritta direttamente sul display: nessuna libreria grafica,
// quindi nessun conflitto con i moduli di sistema che servono per installare). Senza, durante l'installazione lo schermo resta nero.
static void show_screen(void) {
  SceUID blk = sceKernelAllocMemBlock("wa_upd_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 2 * 1024 * 1024, NULL); if (blk < 0) return;
  void *base = NULL; if (sceKernelGetMemBlockBase(blk, &base) < 0 || !base) return;
  int fd = sceIoOpen("app0:/assets/update-screen.raw", SCE_O_RDONLY, 0); if (fd < 0) return;
  size_t total = 960 * 544 * 4, got = 0; while (got < total) { int n = sceIoRead(fd, (char *)base + got, total - got); if (n <= 0) break; got += (size_t)n; }
  sceIoClose(fd); if (got < total) return;
  SceDisplayFrameBuf fb; memset(&fb, 0, sizeof fb); fb.size = sizeof fb; fb.base = base; fb.pitch = 960; fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8; fb.width = 960; fb.height = 544;
  sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}
int main(void) {
  sceAppMgrDestroyOtherApp();
  show_screen();
  int result = promote_app();
  if (result < 0) report_failure(result); else sceIoRemove(FAILURE_FILE);
  sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=" TITLE_ID);
  return 0;
}
