#ifndef VITA_SW_DECODER_H
#define VITA_SW_DECODER_H

#include <stddef.h>
#include <stdint.h>

/* A factory must create a new independent cursor every time open() is called.
 * The player opens two cursors for a muxed file so audio and video can demux
 * concurrently without sharing seek state. */
typedef struct VitaSwDecoderStreamHandle {
	void *opaque;
	int (*read)(void *opaque, void *buffer, size_t size);
	int64_t (*seek)(void *opaque, int64_t offset, int whence);
	void (*close)(void *opaque);
	int64_t size;
	/* Optional non-owning wake-up invoked concurrently while read/seek is active
	 * and while the decoder's input lock is held. It must be prompt, nonblocking,
	 * and idempotent; it must not free opaque, call close, or re-enter decoder
	 * APIs. A transport may leave the cursor unusable because the decoder reopens
	 * it before any operation that would otherwise reuse it. */
	void (*abort)(void *opaque);
} VitaSwDecoderStreamHandle;

typedef struct VitaSwDecoderStreamFactory {
	void *opaque;
	int (*open)(void *opaque, VitaSwDecoderStreamHandle *out);
	/* Optional cancellation-aware variant. Zero/designated-initialize the whole
	 * factory so this remains NULL when unused; when supplied, the decoder
	 * prefers it for every cursor open. */
	int (*open_with_cancel)(void *opaque, volatile int *cancel_flag,
	                        VitaSwDecoderStreamHandle *out);
} VitaSwDecoderStreamFactory;

typedef struct VitaSwDecoderPlayer VitaSwDecoderPlayer;

#define VITA_SW_DECODER_MAX_AUDIO_TRACKS 16
#define VITA_SW_DECODER_MAX_SUBTITLE_TRACKS 16
#define VITA_SW_DECODER_AUDIO_CHANGE_ROLLED_BACK 1
#define VITA_SW_DECODER_TRACK_LANGUAGE_SIZE 16
#define VITA_SW_DECODER_TRACK_TITLE_SIZE 64
#define VITA_SW_DECODER_TRACK_CODEC_SIZE 16

/* Fixed-size snapshot copied from the already-open video demux before decode
 * threads start. Text fields contain UTF-8 and are always NUL-terminated. */
typedef struct VitaSwDecoderTrackInfo {
	int32_t stream_index;
	int32_t is_default;
	int32_t channels;
	char language[VITA_SW_DECODER_TRACK_LANGUAGE_SIZE];
	char title[VITA_SW_DECODER_TRACK_TITLE_SIZE];
	char codec[VITA_SW_DECODER_TRACK_CODEC_SIZE];
} VitaSwDecoderTrackInfo;

typedef struct VitaSwDecoderPlayerConfig {
	VitaSwDecoderStreamFactory stream;
	/* Zero-based AAC track ordinal. Zero preserves the historical behavior of
	 * selecting the first playable audio stream. */
	int audio_track;
	uint32_t expected_width;
	uint32_t expected_height;
	int expected_fps;
	uint64_t start_position_ms;
	int volume_percent;
	/* Optional cooperative cancellation flag used during remote opens and
	 * decode. The caller must keep it alive until close returns. */
	volatile int *cancel_flag;
} VitaSwDecoderPlayerConfig;

typedef struct VitaSwDecoderPlayerStatus {
	int opened;
	int paused;
	int eof;
	int error;
	int hardware_accelerated;
	int direct_rendering;
	int ready_frames;
	int frame_capacity;
	int fps;
	uint32_t width;
	uint32_t height;
	uint64_t video_bitrate_bps;
	uint64_t position_ms;
	uint64_t duration_ms;
	unsigned int frames_decoded;
	unsigned int frames_shown;
	unsigned int frames_dropped;
} VitaSwDecoderPlayerStatus;

/* Convenience factory for a normal Vita path. The path must remain valid
 * until the player is closed. */
void vita_sw_decoder_file_stream_factory(const char *path,
	                             VitaSwDecoderStreamFactory *factory);

/* Loads the packaged decoder compatibility runtime once for the process.
 * Normal player open calls prepare it automatically; embedding applications
 * may call this before creating a separate SceAvPlayer instance. */
int vita_sw_decoder_prepare_runtime(void);
const char *vita_sw_decoder_backend_name(void);

VitaSwDecoderPlayer *vita_sw_decoder_create(void);
int vita_sw_decoder_open(VitaSwDecoderPlayer *player,
	                    const VitaSwDecoderPlayerConfig *config);
void vita_sw_decoder_close(VitaSwDecoderPlayer *player);
void vita_sw_decoder_destroy(VitaSwDecoderPlayer *player);

void vita_sw_decoder_set_paused(VitaSwDecoderPlayer *player, int paused);
void vita_sw_decoder_set_volume(VitaSwDecoderPlayer *player, int percent);
void vita_sw_decoder_request_stop(VitaSwDecoderPlayer *player);

/* Marks the next seek/close as safe to run on a background worker without a
 * vita2d fence. Before calling, the owner must finish every queued draw that
 * references this player and prevent new present calls until the background
 * operation returns. The mark is consumed by that operation. */
void vita_sw_decoder_prepare_background_restart(VitaSwDecoderPlayer *player);

/* Seeks both existing independent cursors to the requested media timestamp. */
int vita_sw_decoder_seek(VitaSwDecoderPlayer *player, uint64_t position_ms);

/* Seeks the existing audio cursor to position_ms with another AAC track while
 * preserving the current pause and volume state. */
int vita_sw_decoder_select_audio_track(VitaSwDecoderPlayer *player,
	                                   int audio_track,
	                                   uint64_t position_ms);
/* Cancellation-aware form used by responsive UIs. The operation flag aborts
 * the forward change; rollback uses a fresh internal phase and remains
 * interruptible through interrupt_audio_operation() or request_stop(). The
 * previous track is restored when possible, and video stays alive on failure.
 * Returns VITA_SW_DECODER_AUDIO_CHANGE_ROLLED_BACK when the requested track
 * failed but the previous track is live again. */
int vita_sw_decoder_select_audio_track_with_cancel(
	VitaSwDecoderPlayer *player, int audio_track, uint64_t position_ms,
	volatile int *operation_cancel);
void vita_sw_decoder_interrupt_audio_operation(VitaSwDecoderPlayer *player);

/* Immutable snapshot from the successful open, cleared by close. The index
 * passed to *_track_info is the zero-based playable-track ordinal. */
int vita_sw_decoder_audio_track_count(const VitaSwDecoderPlayer *player);
int vita_sw_decoder_subtitle_track_count(const VitaSwDecoderPlayer *player);
int vita_sw_decoder_audio_track_info(const VitaSwDecoderPlayer *player,
	                                 int index, VitaSwDecoderTrackInfo *info);
int vita_sw_decoder_subtitle_track_info(const VitaSwDecoderPlayer *player,
	                                    int index, VitaSwDecoderTrackInfo *info);

/* Call present() inside a vita2d drawing scene, then render_complete() after
 * vita2d_wait_rendering_done(). */
int vita_sw_decoder_present(VitaSwDecoderPlayer *player, int fill_screen);
int vita_sw_decoder_present_rect(VitaSwDecoderPlayer *player,
	                            float x, float y, float width, float height,
	                            int fill_rect);
/* Advance the playback clock and discard decoded video frames that are already
 * due without submitting GXM work. The owner must first fence any preceding
 * draw and exclude concurrent present/render_complete calls for this player. */
void vita_sw_decoder_discard_video_to_clock(VitaSwDecoderPlayer *player);
void vita_sw_decoder_render_complete(VitaSwDecoderPlayer *player);
void vita_sw_decoder_get_status(VitaSwDecoderPlayer *player,
	                           VitaSwDecoderPlayerStatus *status);

#endif
