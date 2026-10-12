#ifndef VITA_SW_DECODER_ELEMENTARY_AUDIO_H
#define VITA_SW_DECODER_ELEMENTARY_AUDIO_H

#include <stdint.h>

#include <psp2/kernel/threadmgr.h>

#include <libavformat/avformat.h>

#include "internal/vita_aac_decoder.h"

#define ELEMENTARY_AUDIO_RING_GRAINS 8
#define ELEMENTARY_AUDIO_GRAIN_FRAMES 1024
#define ELEMENTARY_AUDIO_MAX_CHANNELS 2

/* Reads AAC packets from an AVFormatContext owned by the caller, decodes them
 * with sceAudiodec and sends PCM through blocking sceAudioOutOutput. The
 * submitted PTS is the player audio master clock. */

typedef struct {
	AVFormatContext *demux;   /* owned by the caller, NOT closed here */
	int stream_index;
	VitaAacDecoder decoder;
	volatile int *cancel;      /* shared with the caller, not owned */
	volatile int *start_gate;  /* decoder is ready, output starts only on GO */
	volatile int had_error;
	volatile int eof;
	/* Published before the start gate opens once the worker has retained an AU at
	 * or after initial_position_ms. Track replacement can therefore reject an
	 * empty tail without allowing the new AudioOut cursor to become live. */
	volatile int first_packet_ready;
	/* Timestamp of the AU retained before the shared start gate opens. Seek
	 * transactions use it to prevent stale audio from being released beside a
	 * correctly repositioned video frame. */
	volatile uint32_t first_packet_pts_ms;
	volatile int first_packet_pts_valid;
	/* ARM32 can publish a 32-bit millisecond clock atomically; the previous
	 * volatile uint64_t could tear between the audio and render threads. */
	volatile uint32_t played_until_ms;
	uint32_t sample_rate;
	AVPacket *prefetched_packet; /* first probed TS AU, consumed by the worker */
	int input_is_adts;
	volatile int paused;       /* the caller raises/lowers it, the thread honors it */
	volatile int stop;         /* restart only this audio pipeline */
	volatile int volume_percent; /* shared 0..300 perceived volume */
	uint64_t initial_position_ms; /* progressive restart: discard earlier AUs */
	int64_t timeline_origin_us;   /* common container origin, preserves A/V offset */
	int port;
	SceUID thid;
	SceUID output_thid;
	/* The decoder producer stays ahead of the realtime AudioOut consumer. This
	 * bounded ring prevents transport/decode bursts from opening audible gaps. */
	int16_t *pcm_ring; /* 64-byte aligned AudioOut storage */
	uint32_t pcm_end_ms[ELEMENTARY_AUDIO_RING_GRAINS];
	uint8_t pcm_pts_valid[ELEMENTARY_AUDIO_RING_GRAINS];
	volatile uint32_t pcm_write_seq;
	volatile uint32_t pcm_read_seq;
	volatile int producer_done;
	int output_started;
	int decoder_ready; /* guard for vita_sw_aac_decoder_term(), see elementary_audio.c */
	int port_open;      /* guard for sceAudioOutReleasePort() */
	int started;        /* guard for sceKernelWaitThreadEnd()/DeleteThread() */
	volatile int thread_done; /* cooperative-stop grace before transport abort */
} ElementaryAudioState;

/* Finds the first audio track in `demux` (if stream_index < 0) or uses the
 * one given, initializes the AAC decoder with the codecpar parameters and
 * starts the read/decode/output thread. Returns 0 on success, <0 on error (no
 * resource stays allocated on failure). */
int vita_sw_elementary_audio_start(ElementaryAudioState *st, AVFormatContext *demux,
                           int stream_index, uint64_t initial_position_ms,
                           volatile int *cancel,
                           volatile int *start_gate);

/* Waits for the thread to end (because of cancel/eof/had_error), closes the
 * audio port and the decoder. Safe even if vita_sw_elementary_audio_start() was never
 * called or failed (zeroed state). */
void vita_sw_elementary_audio_join(ElementaryAudioState *st);

/* Last pts (microseconds) actually sent to sceAudioOutOutput. Used by the
 * video thread as the synchronization clock. */
uint64_t vita_sw_elementary_audio_clock_us(const ElementaryAudioState *st);

void vita_sw_elementary_audio_set_volume(ElementaryAudioState *st, int percent);

#endif /* VITA_SW_DECODER_ELEMENTARY_AUDIO_H */
