#include "player.h"
#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/sysmodule.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <malloc.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

static SceAvPlayerHandle h; static int open_ok, running, audio_port = -1, audio_thr_on;
static pthread_t audio_thr; static vita2d_texture *tex; static unsigned tex_w, tex_h;

static void *a_alloc(void *arg, uint32_t al, uint32_t sz) { (void)arg; return memalign(al < 16 ? 16 : al, sz); }
static void a_free(void *arg, void *p) { (void)arg; free(p); }
// i fotogrammi li scrive il decodificatore hardware: serve memoria CDRAM visibile al GPU (come negli esempi di SceAvPlayer)
static struct { void *base; SceUID uid; } tex_blk[24];
static void *t_alloc(void *arg, uint32_t al, uint32_t sz) {
  (void)arg; (void)al; sz = (sz + 0x3FFFF) & ~0x3FFFFu;
  SceUID u = sceKernelAllocMemBlock("avp_frame", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, sz, NULL); if (u < 0) return NULL;
  void *b = NULL; if (sceKernelGetMemBlockBase(u, &b) < 0 || !b) { sceKernelFreeMemBlock(u); return NULL; }
  sceGxmMapMemory(b, sz, SCE_GXM_MEMORY_ATTRIB_READ);
  for (int i = 0; i < 24; i++) if (!tex_blk[i].base) { tex_blk[i].base = b; tex_blk[i].uid = u; return b; }
  sceGxmUnmapMemory(b); sceKernelFreeMemBlock(u); return NULL;
}
static void t_free(void *arg, void *p) {
  (void)arg; for (int i = 0; i < 24; i++) if (tex_blk[i].base == p) { vita2d_wait_rendering_done(); sceGxmUnmapMemory(p); sceKernelFreeMemBlock(tex_blk[i].uid); tex_blk[i].base = NULL; return; }
}
static unsigned frames_seen; static int seen_active; static unsigned long long open_at;

static void *audio_loop(void *arg) {
  (void)arg; SceAvPlayerFrameInfo fi;
  while (running) {
    if (sceAvPlayerGetAudioData(h, &fi) && fi.pData) {
      int ch = fi.details.audio.channelCount ? fi.details.audio.channelCount : 2; unsigned grain = fi.details.audio.size / (ch * 2);
      if (audio_port < 0 && grain >= 64) audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, (int)grain, (int)fi.details.audio.sampleRate, ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
      if (audio_port >= 0) sceAudioOutOutput(audio_port, fi.pData);
    } else usleep(4000);
  }
  return NULL;
}

int player_open(const char *path, int loop) {
  player_close();
  sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
  SceAvPlayerInitData d; memset(&d, 0, sizeof d);
  d.memoryReplacement.allocate = a_alloc; d.memoryReplacement.deallocate = a_free; d.memoryReplacement.allocateTexture = t_alloc; d.memoryReplacement.deallocateTexture = t_free;
  d.basePriority = 0xA0; d.numOutputVideoFrameBuffers = 3; d.autoStart = 1; d.debugLevel = 0;
  h = sceAvPlayerInit(&d); if (!h || ((unsigned)h >> 16) == 0x806A) { int er = (int)h; h = 0; return er ? er : -1; }   // l'handle e' un puntatore (anche con il bit alto acceso): errori solo 0 e 0x806Axxxx
  int ar = sceAvPlayerAddSource(h, path); if (ar < 0) { sceAvPlayerClose(h); h = 0; return ar; }
  sceAvPlayerSetLooping(h, loop ? 1 : 0);
  frames_seen = 0; seen_active = 0; open_at = sceKernelGetProcessTimeWide(); open_ok = 1; running = 1; audio_thr_on = pthread_create(&audio_thr, NULL, audio_loop, NULL) == 0;
  return 0;
}

void player_close(void) {
  if (!open_ok) return;
  running = 0; if (audio_thr_on) pthread_join(audio_thr, NULL); audio_thr_on = 0;
  sceAvPlayerStop(h); sceAvPlayerClose(h); h = 0; open_ok = 0;
  if (audio_port >= 0) { sceAudioOutReleasePort(audio_port); audio_port = -1; }
  if (tex) { vita2d_wait_rendering_done(); vita2d_free_texture(tex); tex = NULL; } tex_w = tex_h = 0;
}

int player_active(void) { return open_ok; }

static void *cur_data;
vita2d_texture *player_frame(void) {
  if (!open_ok) return NULL;
  SceAvPlayerFrameInfo fi;
  if (sceAvPlayerGetVideoData(h, &fi) && fi.pData) {
    unsigned w = fi.details.video.width, hh = fi.details.video.height;
    if (w && hh) {
      if (!tex) tex = vita2d_create_empty_texture(8, 8);   // serve solo come contenitore del descrittore GPU
      if (tex) {
        sceGxmTextureInitLinear(&tex->gxm_tex, fi.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, w, hh, 0);
        sceGxmTextureSetMinFilter(&tex->gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR); sceGxmTextureSetMagFilter(&tex->gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
        tex_w = w; tex_h = hh; cur_data = fi.pData; frames_seen++;
      }
    }
  }
  return frames_seen ? tex : NULL;
}
unsigned player_frames(void) { return frames_seen; }
unsigned long long player_time_ms(void) { return open_ok ? (unsigned long long)sceAvPlayerCurrentTime(h) : 0; }
int player_ended(void) {
  if (!open_ok) return 0;
  if (sceAvPlayerIsActive(h)) { seen_active = 1; return 0; }
  if (seen_active) return 1;   // era partito ed ora si e' fermato = finito
  return sceKernelGetProcessTimeWide() - open_at > 15ULL * 1000000ULL;   // mai partito entro 15 s: rinuncia
}
