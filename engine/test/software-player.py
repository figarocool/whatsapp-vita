#!/usr/bin/env python3
"""Exercise the real async player wrapper with a fake decoder and real pthreads.

Vita FFmpeg decoding/rendering is verified separately on the console. This
checks cancellation, error publication, looping and destruction ownership.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
harness = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vita_sw_decoder.h>
#include "software_player.h"
struct VitaSwDecoderPlayer { int frames, eof; };
static atomic_int gate, fail_open, alive, canceled, seeks, opens;
static VitaSwDecoderPlayer *current;
void player_log(const char *s) { (void)s; }
void vita2d_wait_rendering_done(void) {}
VitaSwDecoderPlayer *vita_sw_decoder_create(void) {
  VitaSwDecoderPlayer *p = calloc(1, sizeof(*p)); atomic_fetch_add(&alive, 1); return p;
}
void vita_sw_decoder_file_stream_factory(const char *path, VitaSwDecoderStreamFactory *f) {
  assert(strstr(path, ".mp4")); memset(f, 0, sizeof(*f));
}
int vita_sw_decoder_open(VitaSwDecoderPlayer *p, const VitaSwDecoderPlayerConfig *c) {
  atomic_fetch_add(&opens, 1);
  while (!atomic_load(&gate) && !*c->cancel_flag) usleep(1000);
  if (*c->cancel_flag) { atomic_fetch_add(&canceled, 1); return -99; }
  if (atomic_load(&fail_open)) return -42;
  current = p; return 0;
}
void vita_sw_decoder_destroy(VitaSwDecoderPlayer *p) { free(p); atomic_fetch_sub(&alive, 1); }
void vita_sw_decoder_get_status(VitaSwDecoderPlayer *p, VitaSwDecoderPlayerStatus *s) {
  memset(s, 0, sizeof(*s)); s->opened = 1; s->frames_shown = p->frames;
  s->frames_decoded = 2; s->eof = p->eof; s->position_ms = 40;
}
int vita_sw_decoder_present_rect(VitaSwDecoderPlayer *p, float x, float y, float w, float h, int fill) {
  (void)x; (void)y; (void)w; (void)h; (void)fill; p->frames++; return 1;
}
void vita_sw_decoder_render_complete(VitaSwDecoderPlayer *p) { (void)p; }
void vita_sw_decoder_prepare_background_restart(VitaSwDecoderPlayer *p) { (void)p; }
int vita_sw_decoder_seek(VitaSwDecoderPlayer *p, uint64_t pos) {
  assert(pos == 0); p->eof = 0; atomic_fetch_add(&seeks, 1); return 0;
}
static void wait_picture(void) {
  for (int i = 0; i < 1000; i++) {
    if (software_player_draw(0, 0, 960, 544) == 1) return;
    usleep(1000);
  }
  assert(!"software picture timeout");
}
int main(void) {
  /* A closed viewer must not expose a texture/backend. */
  assert(software_player_draw(0, 0, 10, 10) == -1);
  atomic_store(&gate, 1);
  assert(software_player_open("clip.mp4", 0) == 0); wait_picture();
  assert(software_player_frames() > 0); assert(software_player_time_ms() == 40);
  current->eof = 1; assert(software_player_ended());
  software_player_close(); assert(!atomic_load(&alive));
  /* Cancel while open is blocked: join must finish before destroying. */
  atomic_store(&gate, 0); int before = atomic_load(&opens);
  assert(software_player_open("other.mp4", 0) == 0);
  for (int i = 0; i < 1000 && atomic_load(&opens) == before; i++) usleep(1000);
  assert(atomic_load(&opens) > before);
  assert(software_player_draw(0, 0, 10, 10) == 0);
  software_player_close(); assert(atomic_load(&canceled) == 1); assert(!atomic_load(&alive));
  /* Open failures must publish a failure and be safely destructible. */
  atomic_store(&gate, 1); atomic_store(&fail_open, 1);
  assert(software_player_open("bad.mp4", 0) == 0);
  for (int i = 0; i < 1000 && !software_player_failed(); i++) usleep(1000);
  assert(software_player_failed()); software_player_close(); assert(!atomic_load(&alive));
  /* Loop EOF schedules a background seek rather than decoding on the UI. */
  atomic_store(&fail_open, 0);
  assert(software_player_open("loop.mp4", 1) == 0); wait_picture();
  current->eof = 1; assert(!software_player_ended());
  assert(software_player_draw(0, 0, 10, 10) == 0); wait_picture();
  assert(atomic_load(&seeks) == 1);
  software_player_render_complete(); software_player_close(); assert(!atomic_load(&alive));
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='wa-software-player-') as tmp:
    path = Path(tmp)
    (path / 'vita2d.h').write_text('void vita2d_wait_rendering_done(void);\n')
    (path / 'test.c').write_text(harness)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-DWA_SOFTWARE_VIDEO=1', '-I' + tmp,
                    '-I' + str(root / 'app/src'),
                    '-I' + str(root / 'app/vendor/vita-sw-decoder/include'),
                    str(root / 'app/src/software_player.c'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=10)
print('OK: software player publication, cancellation, failures, looping, teardown')
