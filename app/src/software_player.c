#include "software_player.h"
#include <stdio.h>
#ifdef WA_SOFTWARE_VIDEO
#include <vita_sw_decoder.h>
#include <vita2d.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>

extern void player_log(const char *message);
/* Published state: 0 closed, 1 background open/seek, 2 ready, 3 failed.
 * Only the worker accesses the decoder while state is 1. The UI owns ready
 * sessions. Closing cancels and joins before freeing any decoder or surface. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static VitaSwDecoderPlayer *decoder;
static pthread_t worker;
static int worker_on, state, looping, seek_job, open_error;
static volatile int cancel;
static char source_path[320];
static VitaSwDecoderPlayerStatus last_status;

void log_printf(const char *format, ...) {
  char message[192]; va_list args; va_start(args, format);
  vsnprintf(message, sizeof message, format, args); va_end(args);
  message[strcspn(message, "\r\n")] = 0; player_log(message);
}

static void *open_worker(void *unused) {
  (void)unused;
  int rc;
  if (seek_job) rc = vita_sw_decoder_seek(decoder, 0);
  else {
    decoder = vita_sw_decoder_create();
    VitaSwDecoderPlayerConfig config = {0};
    vita_sw_decoder_file_stream_factory(source_path, &config.stream);
    config.volume_percent = 100; config.cancel_flag = &cancel;
    rc = decoder ? vita_sw_decoder_open(decoder, &config) : -1;
  }
  pthread_mutex_lock(&lock);
  open_error = rc; state = rc == 0 && !cancel ? 2 : 3;
  pthread_mutex_unlock(&lock);
  log_printf("lettore software: %s -> 0x%x", seek_job ? "riavvio" : "apertura", (unsigned)rc);
  return NULL;
}

static int launch_worker(void) {
  pthread_attr_t attr; pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 512 * 1024);
  int rc = pthread_create(&worker, &attr, open_worker, NULL);
  pthread_attr_destroy(&attr); worker_on = rc == 0;
  if (rc != 0) { state = 3; open_error = -rc; }
  return rc == 0 ? 0 : -1;
}

int software_player_open(const char *path, int loop) {
  software_player_close();
  snprintf(source_path, sizeof source_path, "%s", path);
  cancel = 0; looping = loop; seek_job = 0; state = 1; open_error = 0;
  memset(&last_status, 0, sizeof last_status);
  log_printf("lettore: fallback FFmpeg software H.264");
  return launch_worker();
}

void software_player_close(void) {
  cancel = 1;
  if (worker_on) { pthread_join(worker, NULL); worker_on = 0; }
  if (decoder) {
    vita2d_wait_rendering_done(); vita_sw_decoder_render_complete(decoder);
    vita_sw_decoder_destroy(decoder); decoder = NULL;
  }
  state = 0; memset(&last_status, 0, sizeof last_status);
}

static int snapshot(void) {
  pthread_mutex_lock(&lock); int ready = state; pthread_mutex_unlock(&lock);
  if (ready == 2) vita_sw_decoder_get_status(decoder, &last_status);
  return ready;
}
int software_player_active(void) { return snapshot() != 0; }
int software_player_failed(void) { return snapshot() == 3 || last_status.error; }
unsigned software_player_frames(void) { snapshot(); return last_status.frames_shown; }
unsigned long long software_player_time_ms(void) { snapshot(); return last_status.position_ms; }
int software_player_ended(void) { snapshot(); return !looping && last_status.eof; }

int software_player_draw(float x, float y, float w, float h) {
  int ready = snapshot(); if (!ready) return -1;
  if (ready != 2 || last_status.error) return 0;
  if (looping && last_status.eof) {
    vita2d_wait_rendering_done(); vita_sw_decoder_render_complete(decoder);
    vita_sw_decoder_prepare_background_restart(decoder);
    if (worker_on) { pthread_join(worker, NULL); worker_on = 0; }
    pthread_mutex_lock(&lock); state = 1; pthread_mutex_unlock(&lock);
    seek_job = 1; launch_worker(); return 0;
  }
  return vita_sw_decoder_present_rect(decoder, x, y, w, h, 0);
}
void software_player_render_complete(void) {
  if (snapshot() == 2) {
    vita2d_wait_rendering_done(); vita_sw_decoder_render_complete(decoder);
  }
}
void software_player_diag(char *out, int n) {
  int current = snapshot();
  snprintf(out, n, "FFmpeg CPU: stato=%d errore=0x%x %ux%u decodificati=%u mostrati=%u scartati=%u", current, (unsigned)open_error, last_status.width, last_status.height, last_status.frames_decoded, last_status.frames_shown, last_status.frames_dropped);
}
#else
int software_player_open(const char *path, int loop) { (void)path; (void)loop; return -1; }
void software_player_close(void) {}
int software_player_active(void) { return 0; }
int software_player_draw(float x, float y, float w, float h) { (void)x; (void)y; (void)w; (void)h; return -1; }
void software_player_render_complete(void) {}
unsigned software_player_frames(void) { return 0; }
unsigned long long software_player_time_ms(void) { return 0; }
int software_player_ended(void) { return 0; }
int software_player_failed(void) { return 1; }
void software_player_diag(char *out, int n) { snprintf(out, n, "FFmpeg software non incluso"); }
#endif
