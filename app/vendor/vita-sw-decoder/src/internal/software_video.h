#ifndef VITA_SW_DECODER_SOFTWARE_VIDEO_H
#define VITA_SW_DECODER_SOFTWARE_VIDEO_H

#include <stdint.h>
#include <pthread.h>

#include <psp2/kernel/threadmgr.h>
#include <psp2/types.h>
#include <vita2d.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

/* Hardware decoding retains no CPU copy of a ready picture, so it can use a
 * deeper presentation queue than the software fallback without multiplying
 * the latter's CDRAM cost.  At 60 fps sixteen slots cover about 267 ms; the
 * eight-slot copied ring remains the bounded CPU fallback. */
#define SOFTWARE_VIDEO_FRAME_SLOTS 16
#define SOFTWARE_VIDEO_COPY_FRAME_SLOTS 8
#define SOFTWARE_VIDEO_ZERO_COPY_SURFACES 20

typedef struct SoftwareVideoZeroSurface {
	void *pixels;
	SceUID memblock;
	uint32_t allocation_size;
	volatile int in_use;
} SoftwareVideoZeroSurface;

typedef struct {
	void *pixels;
	SceUID memblock;
	uint32_t allocation_size;
	/* Zero-copy mode retains the AVFrame/AVBufferRef handed out by
	 * get_buffer2(). Copy mode leaves this NULL and owns `pixels` directly. */
	AVFrame *frame;
	uint64_t pts_us;
	uint32_t serial;
	int state;
} SoftwareVideoFrameSlot;

/* H.264 decoder for a seekable or progressively supplied media stream. The preferred
 * FFmpeg codec is h264_vita (dedicated Vita video engine); the original
 * multi-threaded CPU codec remains an in-process fallback. Hardware frames can
 * be rendered directly from a bounded CDRAM NV12 pool; the fallback copies
 * planar YUV420 into its independent ring. No RGBA conversion or container rewrite is
 * involved. */
typedef struct {
	AVFormatContext *demux;          /* owned by the caller */
	int stream_index;
	AVCodecContext *decoder;
	volatile int *cancel;
	volatile int *start_gate;
	AVRational time_base;

	uint32_t content_width;
	uint32_t content_height;
	uint32_t texture_width;
	uint32_t texture_height;
	uint32_t pitch;
	uint32_t frame_bytes;
	uint32_t ring_memory_bytes;
	int hardware_direct_available;
	int zero_copy;
	int hardware_runtime_ready;
	int hardware_accelerated;
	int source_fps;
	int continuous_high_fps;
	int queue_capacity;
	uint64_t initial_position_ms;
	int initial_preroll_complete;

	SoftwareVideoFrameSlot slots[SOFTWARE_VIDEO_FRAME_SLOTS];
	SoftwareVideoZeroSurface zero_surfaces[SOFTWARE_VIDEO_ZERO_COPY_SURFACES];
	volatile int slot_lock;
	int display_slot;
	int retire_slot;
	uint32_t next_serial;
	uint64_t observed_clock_us;
	uint64_t hardware_max_pts_us;
	uint64_t last_presented_pts_us;
	unsigned int hardware_pts_repairs;
	unsigned int hardware_pts_missing;
	unsigned int hardware_pts_nonmonotonic;
	int hardware_pts_initialized;
	vita2d_texture texture;

	volatile int had_error;
	volatile int eof;
	volatile int stop;
	unsigned int packets_read;
	volatile unsigned int frames_decoded;
	volatile uint64_t last_decoded_pts_us;
	unsigned int frames_shown;
	unsigned int frames_dropped;
	unsigned int frames_late_dropped;
	unsigned int catchup_switches;
	int catchup_nonref;
	int catchup_drop_active;
	uint64_t catchup_recover_started_us;
	uint64_t pacing_diag_wall_us;
	uint64_t pacing_diag_pts_us;
	unsigned int pacing_diag_frames_shown;
	uint64_t last_presented_wall_us;
	uint32_t presentation_gap_max_us;
	unsigned int presentation_cadence_breaks;
	uint64_t decode_time_us;
	uint64_t copy_time_us;
	uint64_t queue_wait_time_us;
	volatile uint32_t zero_surface_wait_us;

	pthread_t thread;
	volatile int init_done;
	volatile int init_result;
	volatile int thread_done;
	int started;
	/* Set only after a restart join has stopped the worker and the caller has
	 * guaranteed that GXM no longer references any slot. The next same-size
	 * start may reuse the CDRAM/AVFrame pool instead of reallocating it. */
	int buffers_retained;
} SoftwareVideoState;

typedef struct SoftwareVideoDebugSnapshot {
	int hardware_accelerated;
	int direct_rendering;
	int source_fps;
	int ready_frames;
	int have_display;
	int queue_capacity;
	int catchup_nonref;
	uint32_t ring_memory_bytes;
	unsigned int packets_read;
	unsigned int frames_decoded;
	unsigned int frames_published;
	unsigned int frames_shown;
	unsigned int frames_dropped;
	unsigned int frames_late_dropped;
	unsigned int hardware_pts_repairs;
	unsigned int presentation_cadence_breaks;
	uint32_t presentation_gap_max_us;
	uint64_t decoded_pts_us;
	uint64_t decode_time_us;
	uint64_t copy_time_us;
} SoftwareVideoDebugSnapshot;

int vita_sw_software_video_start(SoftwareVideoState *st, AVFormatContext *demux,
	                     int stream_index, uint32_t expected_width,
	                     uint32_t expected_height, int expected_fps,
	                     uint64_t initial_position_ms,
	                     volatile int *cancel,
	                     volatile int *start_gate);

void vita_sw_software_video_join(SoftwareVideoState *st);

/* These variants never call vita2d_wait_rendering_done(). The decoder may use
 * them only after its owner has fenced/quiesced all rendering for this player. */
void vita_sw_software_video_join_for_restart(SoftwareVideoState *st);
void vita_sw_software_video_join_after_gpu_fence(SoftwareVideoState *st);

/* Call between vita2d_start_drawing() and vita2d_end_drawing(). */
int vita_sw_software_video_present(SoftwareVideoState *st,
	                       uint64_t presentation_clock_us,
	                       uint64_t raw_audio_clock_us,
	                       int fill_screen, int allow_advance);

/* Same decoder/pacing path, fitted inside a UI rectangle. Used by the online
 * mini-player so streaming video never needs an RGBA copy or a second scaler. */
int vita_sw_software_video_present_rect(SoftwareVideoState *st,
	                            uint64_t presentation_clock_us,
	                            uint64_t raw_audio_clock_us,
	                            float x, float y, float width, float height,
	                            int fill_rect, int allow_advance);

/* Energy-saving playback keeps demux/decode/audio alive but does not submit
 * video work to GXM. Drop frames that are already behind the audio clock so
 * the decoder ring cannot fill and stall the stream. */
void vita_sw_software_video_discard_to_clock(SoftwareVideoState *st,
	                                 uint64_t audio_clock_us);

void vita_sw_software_video_buffer_status(SoftwareVideoState *st, int *ready_frames,
	                              int *have_display,
	                              uint64_t *display_pts_us);

void vita_sw_software_video_debug_snapshot(SoftwareVideoState *st,
	                               SoftwareVideoDebugSnapshot *snapshot);

/* Call immediately after vita2d_wait_rendering_done(): it returns the
 * previously displayed ring slot to the decoder only after the GPU has
 * finished sampling it. */
void vita_sw_software_video_render_complete(SoftwareVideoState *st);

#endif /* VITA_SW_DECODER_SOFTWARE_VIDEO_H */
