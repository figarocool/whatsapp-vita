#include "player.h"
#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/io/fcntl.h>
#include <psp2/sysmodule.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <malloc.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static SceAvPlayerHandle h; static int open_ok, running, audio_port = -1, audio_thr_on;
static pthread_t audio_thr; static vita2d_texture *tex; static unsigned tex_w, tex_h;

static unsigned ga_n, ga_fail; static unsigned long long ga_bytes;
static volatile int ev_last = -1, ev_src = -1; static volatile unsigned ev_n; static char ev_log[96];
static void ev_cb(void *p, int32_t id, int32_t src, void *data) { (void)p; (void)data; ev_last = id; ev_src = src; if (ev_n < 12) { size_t l = strlen(ev_log); if (l < sizeof ev_log - 8) snprintf(ev_log + l, sizeof ev_log - l, "%d,", (int)id); } ev_n++; }
static void *a_alloc(void *arg, uint32_t al, uint32_t sz) { (void)arg; void *p = memalign(al < 16 ? 16 : al, sz); if (p) { ga_n++; ga_bytes += sz; } else ga_fail++; return p; }
static void a_free(void *arg, void *p) { (void)arg; free(p); }
// i fotogrammi li scrive il decodificatore hardware: serve memoria CDRAM visibile al GPU (come negli esempi di SceAvPlayer)
static unsigned alloc_fail, alloc_ok, alloc_last_fail;
static struct { void *base; SceUID uid; } tex_blk[24];
static void *t_alloc(void *arg, uint32_t al, uint32_t sz) {
  (void)arg; (void)al; sz = (sz + 0x3FFFF) & ~0x3FFFFu;
  SceUID u = sceKernelAllocMemBlock("avp_frame", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, sz, NULL); if (u < 0) { alloc_fail++; alloc_last_fail = sz; return NULL; }
  void *b = NULL; if (sceKernelGetMemBlockBase(u, &b) < 0 || !b) { sceKernelFreeMemBlock(u); return NULL; }
  sceGxmMapMemory(b, sz, SCE_GXM_MEMORY_ATTRIB_READ);
  for (int i = 0; i < 24; i++) if (!tex_blk[i].base) { tex_blk[i].base = b; tex_blk[i].uid = u; alloc_ok++; return b; }
  sceGxmUnmapMemory(b); sceKernelFreeMemBlock(u); return NULL;
}
static void t_free(void *arg, void *p) {
  (void)arg; for (int i = 0; i < 24; i++) if (tex_blk[i].base == p) { vita2d_wait_rendering_done(); sceGxmUnmapMemory(p); sceKernelFreeMemBlock(tex_blk[i].uid); tex_blk[i].base = NULL; return; }
}
static unsigned frames_seen;
static unsigned a_frames; static int a_ch, a_rate, a_size, a_grain, a_open_rc = 1;
 static int seen_active; static unsigned long long open_at;

static void *audio_loop(void *arg) {
  (void)arg; SceAvPlayerFrameInfo fi;
  while (running) {
    if (sceAvPlayerGetAudioData(h, &fi) && fi.pData) {
      int ch = fi.details.audio.channelCount ? fi.details.audio.channelCount : 2; unsigned grain = fi.details.audio.size / (ch * 2);
      a_frames++; a_ch = ch; a_rate = (int)fi.details.audio.sampleRate; a_size = (int)fi.details.audio.size; a_grain = (int)grain;
      if (audio_port < 0) {
        audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, (int)grain, a_rate, ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
        if (audio_port < 0) audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);   // formato standard dei decodificatori Sony
        a_open_rc = audio_port;
        if (audio_port >= 0) { int vol[2] = { SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL }; sceAudioOutSetVolume(audio_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol); }
      }
      if (audio_port >= 0) sceAudioOutOutput(audio_port, fi.pData);
    } else usleep(4000);
  }
  return NULL;
}


// ---- vocali convertiti in WAV dal motore: si riproducono direttamente con sceAudioOut ----
static int wav_mode, wav_port = -1, wav_ch, wav_rate; static uint8_t *wav_buf; static size_t wav_len, wav_pos; static volatile int wav_done, wav_run; static pthread_t wav_thr; static int wav_thr_on;
static int is_wav(const uint8_t *b, size_t n) { return n > 44 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4); }
static char wav_dbg[200];
extern void player_log(const char *m);
static void *wav_loop(void *arg) {
  // si porta sempre tutto a 48000 Hz stereo: e' l'unico formato che la porta principale accetta ovunque
  sceKernelChangeThreadPriority(0, 64); (void)arg; const int grain = 2048; const int nsamp = (int)(wav_len / (wav_ch * 2)); int16_t *src = (int16_t *)wav_buf; int16_t *bufs[2] = { memalign(64, grain * 2 * sizeof(int16_t)), memalign(64, grain * 2 * sizeof(int16_t)) }; int16_t *tmp = bufs[0]; int bi = 0; unsigned long long tprev = 0, gapmax = 0, cmax = 0; unsigned late = 0;
  wav_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, grain, 48000, SCE_AUDIO_OUT_MODE_STEREO);
  snprintf(wav_dbg, sizeof wav_dbg, "vocale: porta audio %d, %d campioni a %d Hz, %d canali", wav_port, nsamp, wav_rate, wav_ch); player_log(wav_dbg);
  if (wav_port >= 0) { int vol[2] = { SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL }; sceAudioOutSetVolume(wav_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol); }
  unsigned long long pos = 0, step = ((unsigned long long)wav_rate << 16) / 48000; unsigned blocks = 0; int mx = 0;
  while (wav_run && wav_port >= 0 && (int)(pos >> 16) < nsamp) {
    unsigned long long tc0 = sceKernelGetProcessTimeWide(); tmp = bufs[bi]; bi ^= 1;
    for (int i = 0; i < grain; i++) {
      int ix = (int)(pos >> 16); float t = (float)(pos & 0xFFFF) / 65536.0f; float o[2] = { 0, 0 };
      if (ix < nsamp) for (int c = 0; c < (wav_ch == 1 ? 1 : 2); c++) {   // interpolazione cubica (Catmull-Rom) tra i campioni: niente scalini udibili
        #define SM(k) ((float)src[(((k) < 0 ? 0 : (k) >= nsamp ? nsamp - 1 : (k))) * wav_ch + c])
        float p0 = SM(ix - 1), p1 = SM(ix), p2 = SM(ix + 1), p3 = SM(ix + 2);
        o[c] = p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
        #undef SM
      }
      if (wav_ch == 1) o[1] = o[0];
      for (int c = 0; c < 2; c++) { if (o[c] > 32767) o[c] = 32767; if (o[c] < -32768) o[c] = -32768; }
      if ((int)o[0] > mx) mx = (int)o[0]; tmp[i * 2] = (int16_t)o[0]; tmp[i * 2 + 1] = (int16_t)o[1]; pos += step;
    }
    unsigned long long tc1 = sceKernelGetProcessTimeWide(); if (tc1 - tc0 > cmax) cmax = tc1 - tc0;
    int orr = sceAudioOutOutput(wav_port, tmp); blocks++; { unsigned long long tn = sceKernelGetProcessTimeWide(); if (tprev) { unsigned long long g = tn - tprev; if (g > gapmax) gapmax = g; if (g > (unsigned long long)grain * 1000000ULL / 48000ULL + 8000) late++; } tprev = tn; } if (orr < 0 && blocks < 3) { snprintf(wav_dbg, sizeof wav_dbg, "vocale: sceAudioOutOutput errore 0x%x", (unsigned)orr); player_log(wav_dbg); }
    wav_pos = (size_t)(pos >> 16) * wav_ch * 2; if (wav_pos > wav_len) wav_pos = wav_len;
  }
  snprintf(wav_dbg, sizeof wav_dbg, "vocale: fine, %u blocchi, picco %d, calcolo max %llu us, giro max %llu us (blocco %d us), giri in ritardo %u", blocks, mx, cmax, gapmax, grain * 1000000 / 48000, late); player_log(wav_dbg);
  free(bufs[0]); free(bufs[1]); wav_pos = wav_len; wav_done = 1; return NULL;
}
static int wav_open(const char *path) {
  SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0); if (f < 0) return -1; SceOff sz = sceIoLseek(f, 0, SCE_SEEK_END); sceIoLseek(f, 0, SCE_SEEK_SET); if (sz < 44 || sz > 40 * 1024 * 1024) { sceIoClose(f); return -1; }
  uint8_t *b = malloc((size_t)sz); if (!b || sceIoRead(f, b, (size_t)sz) != sz) { sceIoClose(f); free(b); return -1; } sceIoClose(f); if (!is_wav(b, (size_t)sz)) { free(b); return -1; }
  size_t i = 12; int ch = 0, rate = 0, bits = 0; size_t doff = 0, dlen = 0;
  while (i + 8 <= (size_t)sz) { uint32_t cl = b[i + 4] | (b[i + 5] << 8) | (b[i + 6] << 16) | ((uint32_t)b[i + 7] << 24); if (!memcmp(b + i, "fmt ", 4) && i + 8 + 16 <= (size_t)sz) { ch = b[i + 10] | (b[i + 11] << 8); rate = b[i + 12] | (b[i + 13] << 8) | (b[i + 14] << 16) | ((uint32_t)b[i + 15] << 24); bits = b[i + 22] | (b[i + 23] << 8); } else if (!memcmp(b + i, "data", 4)) { doff = i + 8; dlen = cl; break; } i += 8 + cl + (cl & 1); }
  if (!doff || (ch != 1 && ch != 2) || bits != 16 || !rate) { free(b); return -1; } if (doff + dlen > (size_t)sz) dlen = (size_t)sz - doff;
  memmove(b, b + doff, dlen); wav_buf = b; wav_len = dlen; wav_pos = 0; wav_ch = ch; wav_rate = rate; wav_done = 0; wav_run = 1; wav_mode = 1; wav_thr_on = pthread_create(&wav_thr, NULL, wav_loop, NULL) == 0; return 0;
}
static void wav_close(void) { if (!wav_mode) return; wav_run = 0; if (wav_thr_on) pthread_join(wav_thr, NULL); wav_thr_on = 0; if (wav_port >= 0) { sceAudioOutReleasePort(wav_port); wav_port = -1; } free(wav_buf); wav_buf = NULL; wav_mode = 0; }

int player_open(const char *path, int loop) {
  player_close();
  if (wav_open(path) == 0) return 0;   // vocale WAV: niente AvPlayer
  sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
  SceAvPlayerInitData d; memset(&d, 0, sizeof d);
  d.memoryReplacement.allocate = a_alloc; d.memoryReplacement.deallocate = a_free; d.memoryReplacement.allocateTexture = t_alloc; d.memoryReplacement.deallocateTexture = t_free;
  d.eventReplacement.eventCallback = ev_cb; ev_n = 0; ev_log[0] = 0; d.basePriority = 0xA0; d.numOutputVideoFrameBuffers = 5; d.autoStart = 1; d.debugLevel = 0;
  h = sceAvPlayerInit(&d); if (!h || ((unsigned)h >> 16) == 0x806A) { int er = (int)h; h = 0; return er ? er : -1; }   // l'handle e' un puntatore (anche con il bit alto acceso): errori solo 0 e 0x806Axxxx
  int ar = sceAvPlayerAddSource(h, path); if (ar < 0) { sceAvPlayerClose(h); h = 0; return ar; }
  sceAvPlayerSetLooping(h, loop ? 1 : 0);
  alloc_ok = alloc_fail = 0; frames_seen = 0; seen_active = 0; open_at = sceKernelGetProcessTimeWide(); open_ok = 1; running = 1; audio_thr_on = pthread_create(&audio_thr, NULL, audio_loop, NULL) == 0;
  return 0;
}

void player_close(void) {
  wav_close();
  if (!open_ok) return;
  running = 0; if (audio_thr_on) pthread_join(audio_thr, NULL); audio_thr_on = 0;
  sceAvPlayerStop(h); sceAvPlayerClose(h); h = 0; open_ok = 0;
  if (audio_port >= 0) { sceAudioOutReleasePort(audio_port); audio_port = -1; }
  if (tex) { vita2d_wait_rendering_done(); vita2d_free_texture(tex); tex = NULL; } tex_w = tex_h = 0;
}

int player_active(void) { return open_ok || wav_mode; }

static void *cur_data;
vita2d_texture *player_frame(void) {
  if (wav_mode || !open_ok) return NULL;
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
unsigned long long player_time_ms(void) { if (wav_mode) return (unsigned long long)wav_pos * 1000ULL / ((unsigned long long)wav_rate * wav_ch * 2); return open_ok ? (unsigned long long)sceAvPlayerCurrentTime(h) : 0; }
int player_ended(void) {
  if (wav_mode) return wav_done;
  if (!open_ok) return 0;
  if (sceAvPlayerIsActive(h)) { seen_active = 1; return 0; }
  if (seen_active) return 1;   // era partito ed ora si e' fermato = finito
  return sceKernelGetProcessTimeWide() - open_at > 15ULL * 1000000ULL;   // mai partito entro 15 s: rinuncia
}
void player_size(unsigned *w, unsigned *h2) { *w = tex_w; *h2 = tex_h; }
void player_audio_diag(char *out, int n) { snprintf(out, n, "audio: frames=%u ch=%d rate=%d size=%d grain=%d porta=%d", a_frames, a_ch, a_rate, a_size, a_grain, a_open_rc); }
void player_video_diag(char *out, int n) { snprintf(out, n, "memoria fotogrammi: ok=%u fallite=%u (ultima %u byte); heap lettore: %u blocchi, %u falliti, %llu KB; attivo=%d", alloc_ok, alloc_fail, alloc_last_fail, ga_n, ga_fail, ga_bytes / 1024, open_ok ? sceAvPlayerIsActive(h) : -1); }
void player_stream_diag(char *out, int n) {
  int w = 0; out[0] = 0; if (!open_ok) return;
  for (uint32_t i = 0; i < 4; i++) { SceAvPlayerStreamInfo si; memset(&si, 0, sizeof si); if (sceAvPlayerGetStreamInfo(h, i, &si) == 0 && w < n - 40) w += snprintf(out + w, n - w, "[flusso %u tipo %u dur %llu ms %ux%u] ", i, si.type, (unsigned long long)si.duration, si.details.video.width, si.details.video.height); }
  if (w < n - 40) snprintf(out + w, n - w, "eventi: %s (n=%u)", ev_log, ev_n);
}
