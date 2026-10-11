#include "audio.h"
#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>
#include <opus/opus.h>

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
int audio_ogg_opus_to_wav(const uint8_t *in, size_t n, uint8_t **out, size_t *outn, double *seconds) {
  if (n < 28 || memcmp(in, "OggS", 4)) return -1;
  ogg_sync_state oy; ogg_stream_state os; ogg_page og; ogg_packet op; OpusDecoder *dec = NULL; int have_stream = 0, packets = 0, channels = 1, preskip = 0, rc = -1;
  const int RATE = 48000; size_t cap = (size_t)RATE * 2 * 4 + 44, len = 44; uint8_t *wav = malloc(cap); if (!wav) return -1; ogg_sync_init(&oy);
  size_t pos = 0; long skip = 0; int done = 0;
  while (!done) {
    while (ogg_sync_pageout(&oy, &og) != 1) {
      if (pos >= n) { done = 1; break; } size_t take = n - pos > 4096 ? 4096 : n - pos; char *buf = ogg_sync_buffer(&oy, (long)take); memcpy(buf, in + pos, take); pos += take; ogg_sync_wrote(&oy, (long)take);
    }
    if (done) break;
    if (!have_stream) { ogg_stream_init(&os, ogg_page_serialno(&og)); have_stream = 1; }
    if (ogg_stream_pagein(&os, &og) < 0) continue;
    while (ogg_stream_packetout(&os, &op) == 1) {
      if (packets == 0) { if (op.bytes < 19 || memcmp(op.packet, "OpusHead", 8)) { rc = -1; done = 1; break; } channels = op.packet[9]; preskip = op.packet[10] | (op.packet[11] << 8); if (channels < 1 || channels > 2) { done = 1; break; } int err; dec = opus_decoder_create(RATE, channels, &err); if (!dec) { done = 1; break; } skip = (long)preskip * RATE / 48000; }
      else if (packets == 1) { /* OpusTags */ }
      else {
        opus_int16 pcm[5760 * 2]; int fr = opus_decode(dec, op.packet, (opus_int32)op.bytes, pcm, 5760, 0);
        if (fr > 0) { for (int i = 0; i < fr; i++) { if (skip > 0) { skip--; continue; } int v = channels == 2 ? (pcm[2 * i] + pcm[2 * i + 1]) / 2 : pcm[i]; if (len + 2 > cap) { cap *= 2; wav = realloc(wav, cap); } wav[len++] = v & 255; wav[len++] = (v >> 8) & 255; } }
      }
      packets++;
    }
  }
  if (dec && len > 44) {
    memcpy(wav, "RIFF", 4); put32(wav + 4, (uint32_t)(len - 8)); memcpy(wav + 8, "WAVEfmt ", 8); put32(wav + 16, 16); wav[20] = 1; wav[21] = 0; wav[22] = 1; wav[23] = 0; put32(wav + 24, RATE); put32(wav + 28, RATE * 2); wav[32] = 2; wav[33] = 0; wav[34] = 16; wav[35] = 0; memcpy(wav + 36, "data", 4); put32(wav + 40, (uint32_t)(len - 44));
    *out = wav; *outn = len; if (seconds) *seconds = (double)(len - 44) / 2.0 / RATE; rc = 0;
  } else free(wav);
  if (dec) opus_decoder_destroy(dec); if (have_stream) ogg_stream_clear(&os); ogg_sync_clear(&oy); return rc;
}
// cerca i marcatori del codec nell'MP4: 'avc1/avc3' = H.264 (ok), 'hvc1/hev1' = HEVC (non riproducibile)
int video_mp4_playable(const uint8_t *in, size_t n) {
  int hevc = 0, avc = 0; for (size_t i = 0; i + 4 <= n; i++) { if (in[i] == 'h' && (!memcmp(in + i, "hvc1", 4) || !memcmp(in + i, "hev1", 4))) hevc = 1; else if (in[i] == 'a' && (!memcmp(in + i, "avc1", 4) || !memcmp(in + i, "avc3", 4))) avc = 1; }
  return hevc && !avc ? 0 : 1;
}

int video_mp4_info(const uint8_t *in, size_t n, Mp4Info *o) {
  memset(o, 0, sizeof *o);
  for (size_t i = 0; i + 40 < n; i++) {
    int avc = !memcmp(in + i, "avc1", 4) || !memcmp(in + i, "avc3", 4), hev = !memcmp(in + i, "hvc1", 4) || !memcmp(in + i, "hev1", 4), vp = !memcmp(in + i, "vp09", 4) || !memcmp(in + i, "av01", 4);
    if (!avc && !hev && !vp) continue;
    { int z = 1; for (int k = 4; k < 10; k++) if (in[i + k]) z = 0; if (!z || in[i + 10] != 0 || in[i + 11] != 1) continue; }   // vale solo dentro 'stsd': a 'avc1' dei marchi iniziali (ftyp) mancano i 6 zeri di una vera voce
    memcpy(o->codec, in + i, 4); o->codec[4] = 0; o->width = (in[i + 28] << 8) | in[i + 29]; o->height = (in[i + 30] << 8) | in[i + 31];   // VisualSampleEntry
    for (size_t j = i + 30; j + 12 < n && j < i + 400; j++) if (!memcmp(in + j, "avcC", 4)) { o->profile = in[j + 5]; o->level = in[j + 7]; break; }
    return 0;
  }
  return -1;
}
