#include "vita_sw_decoder.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>

#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>

#include "decoder_runtime.h"
#include "internal/elementary_audio.h"
#include "internal/software_video.h"
#include "module_log.h"

#ifndef VITA_SW_DECODER_HAS_H264_VITA
#define VITA_SW_DECODER_HAS_H264_VITA 0
#endif

#define VITA_SW_DECODER_AVIO_BUFFER_SIZE (64 * 1024)
#define VITA_SW_DECODER_CLOCK_WINDOW_US 32000ULL
#define VITA_SW_DECODER_OPEN_DEADLINE_US (5ULL * 1000ULL * 1000ULL)
#define VITA_SW_DECODER_AUDIO_READY_DEADLINE_US (1500ULL * 1000ULL)
#define VITA_SW_DECODER_IO_STOP_GRACE_US (30ULL * 1000ULL)
#define VITA_SW_DECODER_AUDIO_START_THREAD_PRIORITY 0x10000100
#define VITA_SW_DECODER_AUDIO_START_THREAD_STACK 0x40000
#define VITA_SW_DECODER_SEEK_OVERSHOOT_US (2ULL * 1000ULL * 1000ULL)
#define VITA_SW_DECODER_AV_LANDING_TOLERANCE_US (250ULL * 1000ULL)

typedef struct {
	VitaSwDecoderStreamHandle stream;
	AVIOContext *avio;
	AVFormatContext *format;
	volatile int *cancel;
	/* Factory transports watch this decoder-owned flag. It can interrupt a
	 * blocked range/socket read for an in-place seek without setting the caller's
	 * session-cancel flag. */
	volatile int transport_cancel;
	uint64_t deadline_us;
	/* abort is non-owning and may race a caller requesting stop with the demux
	 * worker. The lock protects callback lifetime and the active-call handoff;
	 * abort_invoked permanently marks this cursor as unsafe for reuse. */
	volatile int io_lock;
	volatile int io_active;
	volatile int abort_invoked;
	volatile unsigned int cancel_serial;
} VitaSwDecoderInput;

static int input_interrupted(const VitaSwDecoderInput *input) {
	return !input || input->transport_cancel || input->abort_invoked ||
	       (input->cancel && *input->cancel) ||
	       (input->deadline_us &&
	        sceKernelGetProcessTimeWide() >= input->deadline_us);
}

static void input_io_lock(VitaSwDecoderInput *input) {
	while (__sync_lock_test_and_set(&input->io_lock, 1))
		sceKernelDelayThread(100);
}

static void input_io_unlock(VitaSwDecoderInput *input) {
	__sync_lock_release(&input->io_lock);
}

static int input_begin_io(VitaSwDecoderInput *input) {
	if (!input) return 0;
	input_io_lock(input);
	if (input_interrupted(input)) {
		input_io_unlock(input);
		return 0;
	}
	input->io_active = 1;
	__sync_synchronize();
	input_io_unlock(input);
	return 1;
}

static void input_end_io(VitaSwDecoderInput *input) {
	if (!input) return;
	input_io_lock(input);
	input->io_active = 0;
	__sync_synchronize();
	input_io_unlock(input);
}

static int input_abort_active_io(VitaSwDecoderInput *input) {
	if (!input) return 0;
	input_io_lock(input);
	if (input->io_active && !input->abort_invoked && input->stream.abort) {
		/* Publish poison before calling out so a woken worker can never clear the
		 * operation flag and seek this transport again. */
		input->abort_invoked = 1;
		__sync_synchronize();
		input->stream.abort(input->stream.opaque);
	}
	int poisoned = input->abort_invoked;
	input_io_unlock(input);
	return poisoned;
}

static void input_publish_cancel(VitaSwDecoderInput *input) {
	if (!input) return;
	input_io_lock(input);
	input->transport_cancel = 1;
	input->cancel_serial++;
	__sync_synchronize();
	input_io_unlock(input);
}

static int input_cancel_and_abort(VitaSwDecoderInput *input) {
	input_publish_cancel(input);
	return input_abort_active_io(input);
}

static int input_abort_after_grace(VitaSwDecoderInput *input,
	                               int worker_started,
	                               volatile int *thread_done,
	                               uint64_t deadline_us) {
	while (worker_started && thread_done && !*thread_done &&
	       sceKernelGetProcessTimeWide() < deadline_us)
		sceKernelDelayThread(1000);
	__sync_synchronize();
	if (worker_started && (!thread_done || !*thread_done))
		return input_abort_active_io(input);
	return input ? input->abort_invoked : 0;
}

static void input_clear_cancel_if_reusable(VitaSwDecoderInput *input) {
	if (!input) return;
	input_io_lock(input);
	if (!input->abort_invoked && !(input->cancel && *input->cancel))
		input->transport_cancel = 0;
	__sync_synchronize();
	input_io_unlock(input);
}

struct VitaSwDecoderPlayer {
	VitaSwDecoderPlayerConfig config;
	VitaSwDecoderInput video_input;
	VitaSwDecoderInput audio_input;
	SoftwareVideoState video;
	ElementaryAudioState audio;
	volatile int local_cancel;
	volatile int *cancel;
	volatile int start_gate;
	int opened;
	int paused;
	uint64_t duration_ms;
	uint64_t video_bitrate_bps;
	uint64_t clock_us;
	uint64_t clock_wall_us;
	int clock_started;
	int has_audio;
	int audio_clock_handed_off;
	VitaSwDecoderTrackInfo audio_tracks[VITA_SW_DECODER_MAX_AUDIO_TRACKS];
	VitaSwDecoderTrackInfo subtitle_tracks[VITA_SW_DECODER_MAX_SUBTITLE_TRACKS];
	int audio_track_count;
	int subtitle_track_count;
	volatile int audio_operation_lock;
	volatile int audio_operation_active;
	volatile int audio_operation_phase;
	volatile int audio_operation_interrupted;
	volatile int video_gpu_fenced;
};

static void audio_operation_lock(VitaSwDecoderPlayer *player) {
	while (__sync_lock_test_and_set(&player->audio_operation_lock, 1))
		sceKernelDelayThread(100);
}

static void audio_operation_unlock(VitaSwDecoderPlayer *player) {
	__sync_lock_release(&player->audio_operation_lock);
}

static int audio_operation_begin(VitaSwDecoderPlayer *player) {
	audio_operation_lock(player);
	if (player->audio_operation_active) {
		audio_operation_unlock(player);
		return 0;
	}
	/* A replacement audio thread remains behind this gate until the operation
	 * is committed under the same lock. A late interrupt can therefore never
	 * make the newly selected live cursor observe transport_cancel. */
	player->start_gate = 0;
	input_clear_cancel_if_reusable(&player->audio_input);
	player->audio_operation_interrupted = 0;
	player->audio_operation_active = 1;
	player->audio_operation_phase = 1;
	__sync_synchronize();
	audio_operation_unlock(player);
	return 1;
}

static void audio_operation_end(VitaSwDecoderPlayer *player,
	                            int commit_audio) {
	audio_operation_lock(player);
	input_clear_cancel_if_reusable(&player->audio_input);
	player->audio_operation_interrupted = 0;
	player->audio_operation_active = 0;
	player->audio_operation_phase = 0;
	__sync_synchronize();
	player->start_gate = 1;
	__sync_synchronize();
	player->has_audio = commit_audio != 0;
	audio_operation_unlock(player);
}

static int audio_operation_cancelled(const VitaSwDecoderPlayer *player,
	                                 volatile int *operation_cancel) {
	return player->audio_operation_interrupted ||
	       (operation_cancel && *operation_cancel) ||
	       (player->cancel && *player->cancel);
}

static int audio_operation_commit(VitaSwDecoderPlayer *player,
	                              volatile int *operation_cancel) {
	audio_operation_lock(player);
	if (audio_operation_cancelled(player, operation_cancel)) {
		audio_operation_unlock(player);
		return 0;
	}
	/* Clear the operation-only interrupt before releasing the new worker. Since
	 * interrupt_audio_operation() takes this same lock, it can no longer poison
	 * the committed cursor after active becomes false. */
	input_clear_cancel_if_reusable(&player->audio_input);
	player->audio_operation_interrupted = 0;
	player->audio_operation_active = 0;
	player->audio_operation_phase = 0;
	__sync_synchronize();
	player->start_gate = 1;
	__sync_synchronize();
	player->has_audio = 1;
	audio_operation_unlock(player);
	return 1;
}

static void audio_operation_begin_rollback(VitaSwDecoderPlayer *player) {
	audio_operation_lock(player);
	/* The caller's operation flag describes the failed forward change and may
	 * remain set after the UI dismisses its modal. Consume that phase here and
	 * give rollback a fresh decoder-owned interrupt sentinel. */
	input_clear_cancel_if_reusable(&player->audio_input);
	player->audio_operation_interrupted = 0;
	player->audio_operation_phase = 2;
	__sync_synchronize();
	audio_operation_unlock(player);
}

int vita_sw_decoder_prepare_runtime(void) {
	return vita_sw_decoder_runtime_prepare();
}

const char *vita_sw_decoder_backend_name(void) {
	return "software";
}

static int stream_avio_read(void *opaque, uint8_t *buffer, int size) {
	VitaSwDecoderInput *input = (VitaSwDecoderInput *)opaque;
	if (!input || !input->stream.read || !input_begin_io(input)) return AVERROR_EXIT;
	int ret = input->stream.read(input->stream.opaque, buffer, (size_t)size);
	input_end_io(input);
	if (input_interrupted(input)) return AVERROR_EXIT;
	if (ret == 0) return AVERROR_EOF;
	return ret < 0 ? AVERROR(EIO) : ret;
}

static int64_t stream_avio_seek(void *opaque, int64_t offset, int whence) {
	VitaSwDecoderInput *input = (VitaSwDecoderInput *)opaque;
	if (input_interrupted(input)) return AVERROR_EXIT;
	if (whence == AVSEEK_SIZE) return input->stream.size;
	if (!input->stream.seek) return AVERROR(ENOSYS);
	if (!input_begin_io(input)) return AVERROR_EXIT;
	int64_t ret = input->stream.seek(input->stream.opaque, offset,
	                                 whence & ~AVSEEK_FORCE);
	input_end_io(input);
	return input_interrupted(input) ? AVERROR_EXIT : ret;
}

static int interrupt_cb(void *opaque) {
	VitaSwDecoderInput *input = (VitaSwDecoderInput *)opaque;
	return input_interrupted(input);
}

static int playable_aac_stream(const AVStream *stream) {
	if (!stream || stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
	    stream->codecpar->codec_id != AV_CODEC_ID_AAC) return 0;
	int channels = stream->codecpar->ch_layout.nb_channels;
	/* Known multichannel AAC is outside sceAudiodec's contract. Zero remains a
	 * valid candidate for ADTS inputs: elementary_audio resolves channels/rate
	 * from the first access unit when container metadata is absent. */
	return channels >= 0 && channels <= 2;
}

static int text_subtitle_codec(enum AVCodecID codec) {
	return codec == AV_CODEC_ID_SUBRIP || codec == AV_CODEC_ID_ASS ||
	       codec == AV_CODEC_ID_SSA || codec == AV_CODEC_ID_WEBVTT ||
	       codec == AV_CODEC_ID_MOV_TEXT || codec == AV_CODEC_ID_TEXT ||
	       codec == AV_CODEC_ID_MICRODVD;
}

static size_t valid_utf8_sequence(const unsigned char *data, size_t size) {
	if (!data || !size) return 0;
	if (data[0] <= 0x7FU) return 1;
	if (data[0] >= 0xC2U && data[0] <= 0xDFU && size >= 2 &&
	    (data[1] & 0xC0U) == 0x80U) return 2;
	if (data[0] >= 0xE0U && data[0] <= 0xEFU && size >= 3 &&
	    (data[1] & 0xC0U) == 0x80U && (data[2] & 0xC0U) == 0x80U &&
	    !(data[0] == 0xE0U && data[1] < 0xA0U) &&
	    !(data[0] == 0xEDU && data[1] >= 0xA0U)) return 3;
	if (data[0] >= 0xF0U && data[0] <= 0xF4U && size >= 4 &&
	    (data[1] & 0xC0U) == 0x80U && (data[2] & 0xC0U) == 0x80U &&
	    (data[3] & 0xC0U) == 0x80U &&
	    !(data[0] == 0xF0U && data[1] < 0x90U) &&
	    !(data[0] == 0xF4U && data[1] >= 0x90U)) return 4;
	return 0;
}

static void copy_utf8_text(char *out, size_t out_size, const char *value) {
	if (!out || !out_size) return;
	out[0] = '\0';
	if (!value) return;
	const unsigned char *data = (const unsigned char *)value;
	size_t size = strlen(value);
	size_t read = 0;
	size_t written = 0;
	while (read < size) {
		size_t bytes = valid_utf8_sequence(data + read, size - read);
		if (!bytes) {
			if (written + 1 >= out_size) break;
			out[written++] = '?';
			read++;
			continue;
		}
		if (written + bytes >= out_size) break;
		memcpy(out + written, data + read, bytes);
		written += bytes;
		read += bytes;
	}
	out[written] = '\0';
}

static void copy_metadata(char *out, size_t out_size,
	                      const AVDictionary *metadata, const char *key,
	                      int skip_undefined) {
	if (!out || !out_size) return;
	out[0] = '\0';
	const AVDictionaryEntry *entry = av_dict_get(metadata, key, NULL, 0);
	if (!entry || !entry->value ||
	    (skip_undefined && !strcmp(entry->value, "und"))) return;
	copy_utf8_text(out, out_size, entry->value);
}

static void fill_track_info(VitaSwDecoderTrackInfo *out,
	                        const AVStream *stream, int stream_index) {
	memset(out, 0, sizeof(*out));
	out->stream_index = stream_index;
	out->is_default = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
	out->channels = stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
	              ? stream->codecpar->ch_layout.nb_channels : 0;
	copy_metadata(out->language, sizeof(out->language), stream->metadata,
	              "language", 1);
	copy_metadata(out->title, sizeof(out->title), stream->metadata, "title", 0);
	copy_utf8_text(out->codec, sizeof(out->codec),
	               avcodec_get_name(stream->codecpar->codec_id));
}

static void clear_track_snapshot(VitaSwDecoderPlayer *player) {
	if (!player) return;
	memset(player->audio_tracks, 0, sizeof(player->audio_tracks));
	memset(player->subtitle_tracks, 0, sizeof(player->subtitle_tracks));
	player->audio_track_count = 0;
	player->subtitle_track_count = 0;
}

static void snapshot_tracks(VitaSwDecoderPlayer *player,
	                        const AVFormatContext *format) {
	clear_track_snapshot(player);
	for (unsigned int i = 0; format && i < format->nb_streams; i++) {
		const AVStream *stream = format->streams[i];
		const AVCodecParameters *params = stream->codecpar;
		if (playable_aac_stream(stream) &&
		    player->audio_track_count < VITA_SW_DECODER_MAX_AUDIO_TRACKS) {
			fill_track_info(&player->audio_tracks[player->audio_track_count++],
			                stream, (int)i);
		} else if (params->codec_type == AVMEDIA_TYPE_SUBTITLE &&
		           text_subtitle_codec(params->codec_id) &&
		           player->subtitle_track_count <
		               VITA_SW_DECODER_MAX_SUBTITLE_TRACKS) {
			fill_track_info(
			    &player->subtitle_tracks[player->subtitle_track_count++],
			    stream, (int)i);
		}
	}
}

static int playback_streams_ready(const AVFormatContext *format,
	                              int require_track_metadata) {
	int have_video = 0;
	for (unsigned int i = 0; format && i < format->nb_streams; i++) {
		const AVStream *stream = format->streams[i];
		const AVCodecParameters *params = stream->codecpar;
		if (require_track_metadata &&
		    (params->codec_type == AVMEDIA_TYPE_AUDIO ||
		     params->codec_type == AVMEDIA_TYPE_SUBTITLE) &&
		    params->codec_id == AV_CODEC_ID_NONE) return 0;
		if (params->codec_type == AVMEDIA_TYPE_VIDEO &&
		    params->codec_id == AV_CODEC_ID_H264) {
			if (params->width <= 0 || params->height <= 0 ||
			    stream->time_base.num <= 0 || stream->time_base.den <= 0)
				return 0;
			have_video = 1;
		} else if (params->codec_type == AVMEDIA_TYPE_AUDIO &&
		           params->codec_id == AV_CODEC_ID_AAC &&
		           (params->sample_rate <= 0 ||
		            params->ch_layout.nb_channels <= 0 ||
		            stream->time_base.num <= 0 || stream->time_base.den <= 0)) {
			return 0;
		} else if (require_track_metadata &&
		           params->codec_type == AVMEDIA_TYPE_SUBTITLE &&
		           text_subtitle_codec(params->codec_id) &&
		           (stream->time_base.num <= 0 || stream->time_base.den <= 0)) {
			return 0;
		}
	}
	return have_video;
}

static int indexed_container(const AVFormatContext *format) {
	const char *name = format && format->iformat ? format->iformat->name : NULL;
	return name && (strstr(name, "mov,mp4") || strstr(name, "matroska") ||
	                strstr(name, "webm") || strstr(name, "avi"));
}

static void stream_handle_close(VitaSwDecoderStreamHandle *stream) {
	if (!stream) return;
	if (stream->close) stream->close(stream->opaque);
	memset(stream, 0, sizeof(*stream));
}

static void input_close(VitaSwDecoderInput *input) {
	if (!input) return;
	VitaSwDecoderStreamHandle detached = {0};
	unsigned int close_cancel_serial = 0;
	/* Detach the complete handle while stop uses this same lock. Later abort
	 * requests then see no callback/opaque pair, while close owns the detached
	 * cursor until its sole release below. The lock object itself is never reset. */
	input_io_lock(input);
	close_cancel_serial = input->cancel_serial;
	detached = input->stream;
	memset(&input->stream, 0, sizeof(input->stream));
	input->io_active = 0;
	input->transport_cancel = 1;
	input->abort_invoked = 0;
	__sync_synchronize();
	input_io_unlock(input);
	if (input->format) avformat_close_input(&input->format);
	if (input->avio) {
		av_freep(&input->avio->buffer);
		avio_context_free(&input->avio);
	}
	stream_handle_close(&detached);
	input_io_lock(input);
	input->cancel = NULL;
	input->deadline_us = 0;
	/* Preserve a cancellation published after detachment; audio reopen will pass
	 * it to open_with_cancel instead of losing a late UI interrupt. */
	if (input->cancel_serial == close_cancel_serial)
		input->transport_cancel = 0;
	input->io_active = 0;
	input->abort_invoked = 0;
	__sync_synchronize();
	input_io_unlock(input);
}

static int input_open(VitaSwDecoderInput *input,
	                  const VitaSwDecoderStreamFactory *factory,
	                  volatile int *cancel, const char *label,
	                  int require_track_metadata) {
	if (!input || !factory || (!factory->open && !factory->open_with_cancel))
		return AVERROR(EINVAL);
	uint64_t started_us = sceKernelGetProcessTimeWide();
	input_io_lock(input);
	input->cancel = cancel;
	input->transport_cancel = input->transport_cancel || (cancel && *cancel);
	input->deadline_us = 0;
	input->io_active = 0;
	input->abort_invoked = 0;
	__sync_synchronize();
	input_io_unlock(input);
	if (input_interrupted(input)) return AVERROR_EXIT;
	VitaSwDecoderStreamHandle opened = {0};
	int ret = factory->open_with_cancel
	        ? factory->open_with_cancel(factory->opaque,
	                                    &input->transport_cancel,
	                                    &opened)
	        : factory->open(factory->opaque, &opened);
	uint64_t factory_done_us = sceKernelGetProcessTimeWide();
	if (ret < 0 || !opened.read || !opened.seek) {
		log_printf("decoder input %s: factory=%llu ms ret=%d\n",
		           label ? label : "?",
		           (unsigned long long)((factory_done_us - started_us) / 1000ULL),
		           ret < 0 ? ret : AVERROR(EINVAL));
		stream_handle_close(&opened);
		input_close(input);
		return ret < 0 ? ret : AVERROR(EINVAL);
	}
	input_io_lock(input);
	input->stream = opened;
	memset(&opened, 0, sizeof(opened));
	int interrupted = input_interrupted(input);
	__sync_synchronize();
	input_io_unlock(input);
	if (interrupted) {
		input_close(input);
		return AVERROR_EXIT;
	}
	unsigned char *buffer = av_malloc(VITA_SW_DECODER_AVIO_BUFFER_SIZE);
	if (!buffer) {
		input_close(input);
		return AVERROR(ENOMEM);
	}
	input->avio = avio_alloc_context(buffer, VITA_SW_DECODER_AVIO_BUFFER_SIZE, 0,
	                                 input, stream_avio_read, NULL,
	                                 stream_avio_seek);
	if (!input->avio) {
		av_free(buffer);
		input_close(input);
		return AVERROR(ENOMEM);
	}
	input->avio->seekable = AVIO_SEEKABLE_NORMAL;
	input->format = avformat_alloc_context();
	if (!input->format) {
		input_close(input);
		return AVERROR(ENOMEM);
	}
	input->format->pb = input->avio;
	input->format->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_FAST_SEEK;
	input->format->interrupt_callback.callback = interrupt_cb;
	input->format->interrupt_callback.opaque = input;
	input->deadline_us = sceKernelGetProcessTimeWide() +
	                     VITA_SW_DECODER_OPEN_DEADLINE_US;
	input->format->probesize = 1024 * 1024;
	input->format->max_analyze_duration = 2 * AV_TIME_BASE;
	ret = avformat_open_input(&input->format, NULL, NULL, NULL);
	if (ret < 0) {
		int deadline_expired = input->deadline_us &&
		    sceKernelGetProcessTimeWide() >= input->deadline_us;
		if (ret == AVERROR_EXIT && deadline_expired &&
		    !input->transport_cancel && !(cancel && *cancel))
			ret = AVERROR(ETIMEDOUT);
		log_printf("decoder input %s: factory=%llu ms header=%llu ms ret=%d\n",
		           label ? label : "?",
		           (unsigned long long)((factory_done_us - started_us) / 1000ULL),
		           (unsigned long long)((sceKernelGetProcessTimeWide() - factory_done_us) /
		                                1000ULL), ret);
		input_close(input);
		return ret;
	}
	uint64_t header_done_us = sceKernelGetProcessTimeWide();
	/* Fast indexed files normally expose everything during open. Run bounded
	 * discovery whenever the exact H.264/AAC parameters needed by playback are
	 * incomplete; container type alone is not evidence that they are ready. */
	int needs_probe = !(indexed_container(input->format) &&
	                    playback_streams_ready(input->format,
	                                           require_track_metadata));
	ret = needs_probe ? avformat_find_stream_info(input->format, NULL) : 0;
	uint64_t probe_done_us = sceKernelGetProcessTimeWide();
	int deadline_expired = input->deadline_us &&
	    probe_done_us >= input->deadline_us;
	if (ret < 0 && deadline_expired && !input->transport_cancel &&
	    !(cancel && *cancel) &&
	    playback_streams_ready(input->format, require_track_metadata)) {
		/* The wall bound is a responsiveness guard, not a reason to reject a
		 * container whose required parameters became usable before FFmpeg's
		 * optional discovery finished. */
		/* AVERROR_EXIT can remain latched in AVIO. Accept partial discovery only
		 * after a verified demux seek clears that state and restores a valid packet
		 * position; otherwise a nominally successful open would fail immediately in
		 * the decode thread. */
		input->deadline_us = 0;
		if (input->avio) {
			input->avio->error = 0;
			input->avio->eof_reached = 0;
		}
		int resume_ret = av_seek_frame(input->format, -1, 0,
		                               AVSEEK_FLAG_BACKWARD);
		if (resume_ret >= 0) {
			avformat_flush(input->format);
			log_printf("decoder input %s: probe deadline reached; usable streams repositioned\n",
			           label ? label : "?");
			ret = 0;
		}
	}
	if (ret < 0) {
		if (ret == AVERROR_EXIT && deadline_expired &&
		    !input->transport_cancel && !(cancel && *cancel))
			ret = AVERROR(ETIMEDOUT);
		log_printf("decoder input %s: factory=%llu ms header=%llu ms probe=%llu ms ret=%d\n",
		           label ? label : "?",
		           (unsigned long long)((factory_done_us - started_us) / 1000ULL),
		           (unsigned long long)((header_done_us - factory_done_us) / 1000ULL),
		           (unsigned long long)((probe_done_us - header_done_us) / 1000ULL), ret);
		input_close(input);
		return ret;
	}
	input->deadline_us = 0;
	log_printf("decoder input %s: factory=%llu ms header=%llu ms probe=%llu ms total=%llu ms streams=%u probed=%d\n",
	           label ? label : "?",
	           (unsigned long long)((factory_done_us - started_us) / 1000ULL),
	           (unsigned long long)((header_done_us - factory_done_us) / 1000ULL),
	           (unsigned long long)((probe_done_us - header_done_us) / 1000ULL),
	           (unsigned long long)((probe_done_us - started_us) / 1000ULL),
	           input->format->nb_streams, needs_probe);
	return 0;
}

static int ensure_audio_input_reusable(VitaSwDecoderPlayer *player,
	                                   volatile int *operation_cancel) {
	if (!player) return AVERROR(EINVAL);
	if (player->audio_input.format && !player->audio_input.abort_invoked) {
		input_clear_cancel_if_reusable(&player->audio_input);
		return 0;
	}
	if (audio_operation_cancelled(player, operation_cancel)) return AVERROR_EXIT;
	/* An invoked abort is allowed to tear down the transport while preserving
	 * cursor ownership. Close that cursor exactly once, then obtain a fresh
	 * independent audio cursor before any FFmpeg seek/read reuses it. */
	input_close(&player->audio_input);
	if (audio_operation_cancelled(player, operation_cancel)) return AVERROR_EXIT;
	return input_open(&player->audio_input, &player->config.stream,
	                  player->cancel, "audio-reopen", 0);
}

static void stop_audio_candidate(VitaSwDecoderPlayer *player);

typedef struct {
	ElementaryAudioState *audio;
	AVFormatContext *format;
	int stream_index;
	uint64_t position_ms;
	volatile int *cancel;
	volatile int *start_gate;
	volatile int done;
	int result;
} VitaSwDecoderAudioStartJob;

static int audio_start_may_read(const AVFormatContext *format,
	                            int stream_index) {
	if (!format || stream_index < 0 ||
	    (unsigned int)stream_index >= format->nb_streams) return 0;
	const AVCodecParameters *params = format->streams[stream_index]->codecpar;
	return params->codec_id == AV_CODEC_ID_AAC &&
	       (params->ch_layout.nb_channels <= 0 || params->sample_rate <= 0 ||
	        params->extradata_size <= 0);
}

static int audio_start_thread(SceSize args, void *argp) {
	(void)args;
	VitaSwDecoderAudioStartJob *job =
	    *(VitaSwDecoderAudioStartJob **)argp;
	job->result = vita_sw_elementary_audio_start(
	    job->audio, job->format, job->stream_index, job->position_ms,
	    job->cancel, job->start_gate);
	__sync_synchronize();
	job->done = 1;
	return 0;
}

static int start_audio_candidate(VitaSwDecoderPlayer *player,
	                             int stream_index, uint64_t position_ms,
	                             volatile int *operation_cancel,
	                             uint64_t deadline_us) {
	if (!audio_start_may_read(player->audio_input.format, stream_index))
		return vita_sw_elementary_audio_start(
		    &player->audio, player->audio_input.format, stream_index,
		    position_ms, player->cancel, &player->start_gate);

	VitaSwDecoderAudioStartJob job = {
		.audio = &player->audio,
		.format = player->audio_input.format,
		.stream_index = stream_index,
		.position_ms = position_ms,
		.cancel = player->cancel,
		.start_gate = &player->start_gate,
		.result = AVERROR(EINPROGRESS)
	};
	SceUID thid = sceKernelCreateThread(
	    "VitaSwDecoderAudioStart", audio_start_thread,
	    VITA_SW_DECODER_AUDIO_START_THREAD_PRIORITY,
	    VITA_SW_DECODER_AUDIO_START_THREAD_STACK, 0, 0, NULL);
	if (thid < 0) return thid;
	VitaSwDecoderAudioStartJob *job_ptr = &job;
	int ret = sceKernelStartThread(thid, sizeof(job_ptr), &job_ptr);
	if (ret < 0) {
		sceKernelDeleteThread(thid);
		return ret;
	}
	while (!job.done &&
	       !audio_operation_cancelled(player, operation_cancel) &&
	       sceKernelGetProcessTimeWide() < deadline_us)
		sceKernelDelayThread(1000);
	int timed_out = !job.done &&
	                !audio_operation_cancelled(player, operation_cancel) &&
	                sceKernelGetProcessTimeWide() >= deadline_us;
	if (!job.done) {
		/* This path covers the synchronous ADTS probe performed before the normal
		 * audio worker exists. A separate starter thread lets the caller wake that
		 * blocked cursor at the same overall readiness deadline. */
		player->audio.stop = 1;
		__sync_synchronize();
		input_cancel_and_abort(&player->audio_input);
	}
	sceKernelWaitThreadEnd(thid, NULL, NULL);
	sceKernelDeleteThread(thid);
	if (audio_operation_cancelled(player, operation_cancel)) {
		if (player->audio.started || player->audio.decoder_ready ||
		    player->audio.port_open)
			stop_audio_candidate(player);
		return AVERROR_EXIT;
	}
	if (timed_out) {
		if (player->audio.started || player->audio.decoder_ready ||
		    player->audio.port_open)
			stop_audio_candidate(player);
		return AVERROR(ETIMEDOUT);
	}
	return job.result;
}

static int find_stream(const AVFormatContext *format, enum AVMediaType type,
	                   enum AVCodecID codec, int ordinal) {
	if (ordinal < 0) return -1;
	for (unsigned int i = 0; format && i < format->nb_streams; i++) {
		const AVCodecParameters *params = format->streams[i]->codecpar;
		if (params->codec_type != type || params->codec_id != codec) continue;
		if (ordinal-- == 0) return (int)i;
	}
	return -1;
}

static uint64_t input_duration_ms(const AVFormatContext *format) {
	if (!format || format->duration <= 0) return 0;
	return (uint64_t)format->duration * 1000ULL / AV_TIME_BASE;
}

static int64_t media_timeline_origin_us(const AVFormatContext *format) {
	if (!format) return 0;
	if (format->start_time != AV_NOPTS_VALUE) return format->start_time;
	int64_t earliest = AV_NOPTS_VALUE;
	for (unsigned int i = 0; i < format->nb_streams; i++) {
		const AVStream *stream = format->streams[i];
		if (stream->start_time == AV_NOPTS_VALUE ||
		    stream->time_base.num <= 0 || stream->time_base.den <= 0 ||
		    (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
		     stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
		     stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) ||
		    (stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
		int64_t value = av_rescale_q(stream->start_time, stream->time_base,
		                             AV_TIME_BASE_Q);
		if (earliest == AV_NOPTS_VALUE || value < earliest) earliest = value;
	}
	return earliest == AV_NOPTS_VALUE ? 0 : earliest;
}

static uint64_t stream_logical_end_ms(const AVFormatContext *format,
	                                  int stream_index) {
	if (!format || stream_index < 0 ||
	    (unsigned int)stream_index >= format->nb_streams) return 0;
	const AVStream *stream = format->streams[stream_index];
	if (!stream || stream->duration <= 0 ||
	    stream->start_time == AV_NOPTS_VALUE ||
	    stream->time_base.num <= 0 || stream->time_base.den <= 0) return 0;
	int64_t origin_us = media_timeline_origin_us(format);
	int64_t start_us = av_rescale_q(stream->start_time, stream->time_base,
	                               AV_TIME_BASE_Q);
	int64_t duration_us = av_rescale_q(stream->duration, stream->time_base,
	                                  AV_TIME_BASE_Q);
	if (duration_us <= 0) return 0;
	if ((origin_us > 0 && start_us < INT64_MIN + origin_us) ||
	    (origin_us < 0 && start_us > INT64_MAX + origin_us)) return 0;
	int64_t logical_start_us = start_us - origin_us;
	if (logical_start_us > INT64_MAX - duration_us) return 0;
	int64_t logical_end = logical_start_us + duration_us;
	if (logical_end <= 0) return 0;
	uint64_t logical_end_us = (uint64_t)logical_end;
	/* Millisecond API positions denote the start of that millisecond. Round the
	 * exclusive stream end upward so a final fractional millisecond is playable. */
	return (logical_end_us + 999ULL) / 1000ULL;
}

static void close_session(VitaSwDecoderPlayer *player) {
	if (!player) return;
	player->video.stop = 1;
	player->audio.stop = 1;
	__sync_synchronize();
	input_cancel_and_abort(&player->video_input);
	input_cancel_and_abort(&player->audio_input);
	if (player->video.started || player->video.buffers_retained) {
		if (player->video_gpu_fenced)
			vita_sw_software_video_join_after_gpu_fence(&player->video);
		else
			vita_sw_software_video_join(&player->video);
	}
	if (player->audio.started || player->audio.decoder_ready || player->audio.port_open)
		vita_sw_elementary_audio_join(&player->audio);
	input_close(&player->audio_input);
	input_close(&player->video_input);
	clear_track_snapshot(player);
	player->opened = 0;
	player->start_gate = 0;
	player->audio_clock_handed_off = 0;
	player->video_gpu_fenced = 0;
	player->audio_operation_active = 0;
	player->audio_operation_phase = 0;
	player->audio_operation_interrupted = 0;
	player->video.cancel = NULL;
	player->video.start_gate = NULL;
	player->audio.cancel = NULL;
	player->audio.start_gate = NULL;
	player->cancel = NULL;
	player->config.cancel_flag = NULL;
	player->local_cancel = 0;
}

static void isolate_input_stream(AVFormatContext *format, int stream_index) {
	if (!format || stream_index < 0 ||
	    (unsigned int)stream_index >= format->nb_streams) return;
	/* Each playback cursor has exactly one job. Prevent Matroska from
	 * packetizing alternate audio, subtitles, attachments and cover art while
	 * it searches the cue/index and performs the short keyframe preroll. */
	for (unsigned int i = 0; i < format->nb_streams; ++i)
		format->streams[i]->discard = i == (unsigned int)stream_index
		                          ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
}

static int seek_input_ms(VitaSwDecoderInput *input, int stream_index,
	                     uint64_t position_ms) {
	if (!input || !input->format || stream_index < 0 ||
	    (unsigned int)stream_index >= input->format->nb_streams)
		return AVERROR(EINVAL);
	if (input->abort_invoked) return AVERROR(EIO);
	int seek_stream_index = stream_index;
	AVStream *selected_stream = input->format->streams[stream_index];
	if (selected_stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
	    (selected_stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
		/* Matroska commonly stores cue entries for the primary video track only.
		 * Seeking an AAC stream directly makes libavformat walk clusters until it
		 * reaches the requested timestamp. Always hop through the dense H.264 cue
		 * index, then let the selected audio worker discard only the small GOP-sized
		 * interleave margin. */
		seek_stream_index = find_stream(input->format, AVMEDIA_TYPE_VIDEO,
		                                AV_CODEC_ID_H264, 0);
		if (seek_stream_index < 0) return AVERROR_STREAM_NOT_FOUND;
	}
	/* Custom AVIO keeps a prior AVERROR_EXIT/EOF latched across avio_seek(). A
	 * cooperatively stopped, reusable cursor must clear those flags before the
	 * mandatory demux seek; aborted cursors are rejected above and reopened. */
	if (input->avio) {
		input->avio->error = 0;
		input->avio->eof_reached = 0;
	}
	isolate_input_stream(input->format, seek_stream_index);
	AVStream *stream = input->format->streams[seek_stream_index];
	int64_t logical_us = av_rescale_q((int64_t)position_ms,
	                                  (AVRational){ 1, 1000 }, AV_TIME_BASE_Q);
	int64_t target = av_rescale_q(
	    logical_us + media_timeline_origin_us(input->format),
	    AV_TIME_BASE_Q, stream->time_base);
	/* av_seek_frame on the video stream resolves a Matroska Cue/keyframe byte
	 * offset. Do not fall back to avformat_seek_file or to the selected AAC
	 * stream: either can degrade into a linear cluster scan. */
	int ret = av_seek_frame(input->format, seek_stream_index, target,
	                        AVSEEK_FLAG_BACKWARD);
	if (ret >= 0) avformat_flush(input->format);
	/* Packet reading after the indexed hop belongs solely to the decoder worker
	 * for the originally selected stream. */
	isolate_input_stream(input->format, stream_index);
	log_printf("decoder indexed hop: target=%llu ms anchor=%d selected=%d ret=%d\n",
	           (unsigned long long)position_ms, seek_stream_index, stream_index,
	           ret);
	return ret;
}

static void stop_audio_candidate(VitaSwDecoderPlayer *player) {
	player->audio.stop = 1;
	__sync_synchronize();
	input_publish_cancel(&player->audio_input);
	uint64_t stop_deadline = sceKernelGetProcessTimeWide() +
	                         VITA_SW_DECODER_IO_STOP_GRACE_US;
	input_abort_after_grace(&player->audio_input, player->audio.started,
	                        &player->audio.thread_done, stop_deadline);
	if (player->audio.started || player->audio.decoder_ready ||
	    player->audio.port_open)
		vita_sw_elementary_audio_join(&player->audio);
	input_clear_cancel_if_reusable(&player->audio_input);
	memset(&player->audio, 0, sizeof(player->audio));
}

static int restart_audio_track(VitaSwDecoderPlayer *player, int audio_track,
	                           uint64_t position_ms,
	                           volatile int *operation_cancel,
	                           int require_initial_packet) {
	uint64_t started_us = sceKernelGetProcessTimeWide();
	player->start_gate = 0;
	player->audio.stop = 1;
	__sync_synchronize();
	input_publish_cancel(&player->audio_input);
	uint64_t stop_deadline = sceKernelGetProcessTimeWide() +
	                         VITA_SW_DECODER_IO_STOP_GRACE_US;
	input_abort_after_grace(&player->audio_input, player->audio.started,
	                        &player->audio.thread_done, stop_deadline);
	if (player->audio.started || player->audio.decoder_ready || player->audio.port_open)
		vita_sw_elementary_audio_join(&player->audio);
	uint64_t joined_us = sceKernelGetProcessTimeWide();
	memset(&player->audio, 0, sizeof(player->audio));
	player->has_audio = 0;
	player->audio_clock_handed_off = 0;
	player->clock_us = position_ms * 1000ULL;
	player->clock_wall_us = joined_us;
	player->clock_started = 1;
	if (audio_operation_cancelled(player, operation_cancel))
		return AVERROR_EXIT;
	int ret = ensure_audio_input_reusable(player, operation_cancel);
	if (ret < 0) return ret;
	if (audio_operation_cancelled(player, operation_cancel))
		return AVERROR_EXIT;

	int audio_index = audio_track >= 0 &&
	                          audio_track < player->audio_track_count
	                    ? player->audio_tracks[audio_track].stream_index : -1;
	if (audio_index >= 0 &&
	    (!player->audio_input.format ||
	     (unsigned int)audio_index >= player->audio_input.format->nb_streams ||
	     !playable_aac_stream(player->audio_input.format->streams[audio_index])))
		audio_index = -1;
	if (audio_index < 0) {
		log_printf("decoder audio switch: join=%llu ms total=%llu ms ret=%d\n",
		           (unsigned long long)((joined_us - started_us) / 1000ULL),
		           (unsigned long long)((sceKernelGetProcessTimeWide() - started_us) /
		                                1000ULL), AVERROR_DECODER_NOT_FOUND);
		return AVERROR_DECODER_NOT_FOUND;
	}
	ret = seek_input_ms(&player->audio_input, audio_index, position_ms);
	uint64_t seek_done_us = sceKernelGetProcessTimeWide();
	if (audio_operation_cancelled(player, operation_cancel))
		return AVERROR_EXIT;
	if (ret < 0) {
		log_printf("decoder audio switch: join=%llu ms demux=%llu ms total=%llu ms ret=%d\n",
		           (unsigned long long)((joined_us - started_us) / 1000ULL),
		           (unsigned long long)((seek_done_us - joined_us) / 1000ULL),
		           (unsigned long long)((seek_done_us - started_us) / 1000ULL), ret);
		return ret;
	}
	uint64_t readiness_deadline = sceKernelGetProcessTimeWide() +
	                              VITA_SW_DECODER_AUDIO_READY_DEADLINE_US;
	ret = start_audio_candidate(player, audio_index, position_ms,
	                            operation_cancel, readiness_deadline);
	if (ret < 0) {
		log_printf("decoder audio switch: join=%llu ms demux=%llu ms restart=%llu ms total=%llu ms ret=%d\n",
		           (unsigned long long)((joined_us - started_us) / 1000ULL),
		           (unsigned long long)((seek_done_us - joined_us) / 1000ULL),
		           (unsigned long long)((sceKernelGetProcessTimeWide() - seek_done_us) /
		                                1000ULL),
		           (unsigned long long)((sceKernelGetProcessTimeWide() - started_us) /
		                                1000ULL), ret);
		return ret;
	}
	if (require_initial_packet) {
		/* The audio worker primes one AU without crossing start_gate. Waiting here
		 * keeps the replacement transactional while video continues decoding. The
		 * deadline turns a sparse/malformed tail into a rollback instead of an
		 * unbounded modal; stop_audio_candidate wakes a cooperative transport read. */
		while (!audio_operation_cancelled(player, operation_cancel) &&
		       !player->audio.first_packet_ready && !player->audio.eof &&
		       !player->audio.had_error &&
		       sceKernelGetProcessTimeWide() < readiness_deadline)
			sceKernelDelayThread(1000);
		__sync_synchronize();
	}
	if (audio_operation_cancelled(player, operation_cancel)) {
		stop_audio_candidate(player);
		return AVERROR_EXIT;
	}
	if (require_initial_packet && !player->audio.first_packet_ready) {
		int readiness_ret = player->audio.eof ? AVERROR_EOF
		                    : player->audio.had_error ? AVERROR_INVALIDDATA
		                    : AVERROR(ETIMEDOUT);
		log_printf("decoder audio switch: no AU at %llu ms ret=%d track=%d deadline=%u ms",
		           (unsigned long long)position_ms, readiness_ret, audio_track,
		           (unsigned)(VITA_SW_DECODER_AUDIO_READY_DEADLINE_US / 1000ULL));
		stop_audio_candidate(player);
		return readiness_ret;
	}
	if (require_initial_packet && player->audio.first_packet_pts_valid &&
	    (uint64_t)player->audio.first_packet_pts_ms * 1000ULL >
	        position_ms * 1000ULL + VITA_SW_DECODER_SEEK_OVERSHOOT_US) {
		log_printf("decoder audio switch: invalid landing target=%llu ms audio=%u ms; rejecting\n",
		           (unsigned long long)position_ms,
		           player->audio.first_packet_pts_ms);
		stop_audio_candidate(player);
		return AVERROR_INVALIDDATA;
	}
	vita_sw_elementary_audio_set_volume(&player->audio,
	                                    player->config.volume_percent);
	player->audio.paused = player->paused;
	uint64_t completed_us = sceKernelGetProcessTimeWide();
	log_printf("decoder audio switch: join=%llu ms demux=%llu ms restart=%llu ms total=%llu ms ret=0 track=%d\n",
	           (unsigned long long)((joined_us - started_us) / 1000ULL),
	           (unsigned long long)((seek_done_us - joined_us) / 1000ULL),
	           (unsigned long long)((completed_us - seek_done_us) / 1000ULL),
	           (unsigned long long)((completed_us - started_us) / 1000ULL),
	           audio_track);
	return 0;
}

static void abandon_audio_replacement(VitaSwDecoderPlayer *player,
	                                  uint64_t position_ms) {
	stop_audio_candidate(player);
	player->has_audio = 0;
	player->audio_clock_handed_off = 0;
	player->clock_us = position_ms * 1000ULL;
	player->clock_wall_us = sceKernelGetProcessTimeWide();
	player->clock_started = 1;
	__sync_synchronize();
}

static int restart_session_in_place(VitaSwDecoderPlayer *player,
	                                uint64_t position_ms) {
	uint64_t started_us = sceKernelGetProcessTimeWide();
	int video_index = find_stream(player->video_input.format,
	                              AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264, 0);
	int audio_index = player->has_audio && player->config.audio_track >= 0 &&
	                          player->config.audio_track < player->audio_track_count
	                    ? player->audio_tracks[player->config.audio_track].stream_index
	                    : -1;
	if (audio_index >= 0 &&
	    (!player->audio_input.format ||
	     (unsigned int)audio_index >= player->audio_input.format->nb_streams ||
	     !playable_aac_stream(player->audio_input.format->streams[audio_index])))
		audio_index = -1;
	if (video_index < 0 || (player->has_audio && audio_index < 0)) {
		log_printf("decoder seek: stage=streams total=%llu ms ret=%d\n",
		           (unsigned long long)((sceKernelGetProcessTimeWide() - started_us) /
		                                1000ULL), AVERROR_DECODER_NOT_FOUND);
		return AVERROR_DECODER_NOT_FOUND;
	}
	player->start_gate = 0;
	player->video.stop = 1;
	player->audio.stop = 1;
	__sync_synchronize();
	input_publish_cancel(&player->video_input);
	input_publish_cancel(&player->audio_input);
	uint64_t stop_deadline = sceKernelGetProcessTimeWide() +
	                         VITA_SW_DECODER_IO_STOP_GRACE_US;
	input_abort_after_grace(&player->video_input, player->video.started,
	                        &player->video.thread_done, stop_deadline);
	input_abort_after_grace(&player->audio_input, player->audio.started,
	                        &player->audio.thread_done, stop_deadline);
	if (player->video.started || player->video.buffers_retained) {
		if (player->video_gpu_fenced)
			vita_sw_software_video_join_for_restart(&player->video);
		else
			vita_sw_software_video_join(&player->video);
	}
	if (player->audio.started || player->audio.decoder_ready || player->audio.port_open)
		vita_sw_elementary_audio_join(&player->audio);
	uint64_t joined_us = sceKernelGetProcessTimeWide();
	if (player->cancel && *player->cancel) return AVERROR_EXIT;
	if (player->video_input.abort_invoked ||
	    player->audio_input.abort_invoked) {
		/* abort may shut down the transport (SFTP/SMB do). Leave both cursors
		 * cancelled and force the caller's checked full-session reopen rather than
		 * seeking an AVFormatContext backed by a poisoned cursor. */
		log_printf("decoder seek: stage=join cursor-aborted video=%d audio=%d ret=%d\n",
		           player->video_input.abort_invoked,
		           player->audio_input.abort_invoked, AVERROR(EIO));
		return AVERROR(EIO);
	}
	input_clear_cancel_if_reusable(&player->video_input);
	input_clear_cancel_if_reusable(&player->audio_input);
	int ret = seek_input_ms(&player->video_input, video_index, position_ms);
	uint64_t video_seek_done_us = sceKernelGetProcessTimeWide();
	if (ret >= 0 && player->has_audio) {
		if (position_ms == 0) {
			/* An indexed hop to absolute zero can report success while an
			 * independent audio cursor remains on its previous Matroska cluster.
			 * A fresh cursor is deterministic and this exceptional path is only
			 * used for an actual restart from the beginning. */
			input_close(&player->audio_input);
			ret = player->cancel && *player->cancel ? AVERROR_EXIT
			      : input_open(&player->audio_input, &player->config.stream,
			                   player->cancel, "audio-zero-reopen", 0);
			if (ret >= 0 &&
			    ((unsigned int)audio_index >= player->audio_input.format->nb_streams ||
			     !playable_aac_stream(
			         player->audio_input.format->streams[audio_index])))
				ret = AVERROR_DECODER_NOT_FOUND;
			if (ret >= 0) {
				isolate_input_stream(player->audio_input.format, audio_index);
				log_printf("decoder audio zero: reopened cursor selected=%d ret=0\n",
				           audio_index);
			}
		} else {
			ret = seek_input_ms(&player->audio_input, audio_index, position_ms);
		}
	}
	uint64_t demux_done_us = sceKernelGetProcessTimeWide();
	if (ret < 0) {
		log_printf("decoder seek: stage=demux join=%llu ms video-demux=%llu ms audio-demux=%llu ms total=%llu ms ret=%d\n",
		           (unsigned long long)((joined_us - started_us) / 1000ULL),
		           (unsigned long long)((video_seek_done_us - joined_us) / 1000ULL),
		           (unsigned long long)((demux_done_us - video_seek_done_us) / 1000ULL),
		           (unsigned long long)((demux_done_us - started_us) / 1000ULL), ret);
		return ret;
	}
	memset(&player->audio, 0, sizeof(player->audio));
	player->clock_us = position_ms * 1000ULL;
	player->clock_wall_us = sceKernelGetProcessTimeWide();
	player->clock_started = 0;
	player->audio_clock_handed_off = 0;
	ret = vita_sw_software_video_start(&player->video, player->video_input.format,
	                           video_index, player->config.expected_width,
	                           player->config.expected_height,
	                           player->config.expected_fps, position_ms,
	                           player->cancel, &player->start_gate);
	if (ret < 0) {
		log_printf("decoder seek: stage=video-restart join=%llu ms demux=%llu ms restart=%llu ms total=%llu ms ret=%d\n",
		           (unsigned long long)((joined_us - started_us) / 1000ULL),
		           (unsigned long long)((demux_done_us - joined_us) / 1000ULL),
		           (unsigned long long)((sceKernelGetProcessTimeWide() - demux_done_us) /
		                                1000ULL),
		           (unsigned long long)((sceKernelGetProcessTimeWide() - started_us) /
		                                1000ULL), ret);
		return ret;
	}
	if (player->has_audio) {
		uint64_t audio_start_deadline = sceKernelGetProcessTimeWide() +
		                                VITA_SW_DECODER_AUDIO_READY_DEADLINE_US;
		ret = start_audio_candidate(player, audio_index, position_ms, NULL,
		                            audio_start_deadline);
		if (ret < 0) {
			log_printf("decoder seek: stage=audio-restart join=%llu ms demux=%llu ms restart=%llu ms total=%llu ms ret=%d\n",
			           (unsigned long long)((joined_us - started_us) / 1000ULL),
			           (unsigned long long)((demux_done_us - joined_us) / 1000ULL),
			           (unsigned long long)((sceKernelGetProcessTimeWide() - demux_done_us) /
			                                1000ULL),
			           (unsigned long long)((sceKernelGetProcessTimeWide() - started_us) /
			                                1000ULL), ret);
			return ret;
		}
		vita_sw_elementary_audio_set_volume(&player->audio,
		                                    player->config.volume_percent);
		player->audio.paused = player->paused;
	}
	/* A seek only needs one decoded frame before releasing audio. Waiting for a
	 * multi-frame cushion made every interaction feel like a fresh startup on
	 * the Vita CPU, especially after a distant keyframe seek. */
	uint64_t restarted_us = sceKernelGetProcessTimeWide();
	uint64_t deadline = restarted_us + 4000000ULL;
	int ready = 0;
	while (!*player->cancel && !player->video.had_error && !player->video.eof &&
	       !(player->has_audio && player->audio.had_error) &&
	       sceKernelGetProcessTimeWide() < deadline) {
		vita_sw_software_video_buffer_status(&player->video, &ready, NULL, NULL);
		int audio_ready = !player->has_audio || player->audio.first_packet_ready ||
		                  player->audio.eof;
		if (ready >= 1 && audio_ready) break;
		sceKernelDelayThread(1000);
	}
	__sync_synchronize();
	uint64_t first_ready_pts_us = 0;
	vita_sw_software_video_buffer_status(&player->video, &ready, NULL,
	                                     &first_ready_pts_us);
	if (*player->cancel) return AVERROR_EXIT;
	if (!ready && player->video.had_error) {
		log_printf("decoder seek: stage=first-frame ready=0 ret=%d cause=video-error\n",
		           AVERROR_INVALIDDATA);
		return AVERROR_INVALIDDATA;
	}
	if (!ready && player->video.eof) {
		log_printf("decoder seek: stage=first-frame ready=0 ret=%d cause=eof\n",
		           AVERROR_EOF);
		return AVERROR_EOF;
	}
	if (!ready) {
		log_printf("decoder seek: stage=first-frame ready=0 ret=%d cause=timeout\n",
		           AVERROR(ETIMEDOUT));
		return AVERROR(ETIMEDOUT);
	}
	if (player->has_audio && !player->audio.first_packet_ready &&
	    !player->audio.eof) {
		int audio_ret = player->audio.had_error ? AVERROR_INVALIDDATA
		                : AVERROR(ETIMEDOUT);
		log_printf("decoder seek: stage=first-audio target=%llu ms ret=%d eof=%d error=%d\n",
		           (unsigned long long)position_ms, audio_ret,
		           player->audio.eof, player->audio.had_error);
		return audio_ret;
	}
	uint64_t requested_us = position_ms * 1000ULL;
	if (first_ready_pts_us > requested_us +
	                         VITA_SW_DECODER_SEEK_OVERSHOOT_US) {
		/* Returning a checked failure invokes the existing fresh-session recovery.
		 * Never release audio behind a frame several seconds in the future: that is
		 * the black-screen state observed after a live seek. */
		log_printf("decoder seek: invalid landing target=%llu ms frame=%llu ms overshoot=%llu ms; reopening\n",
		           (unsigned long long)position_ms,
		           (unsigned long long)(first_ready_pts_us / 1000ULL),
		           (unsigned long long)((first_ready_pts_us - requested_us) / 1000ULL));
		return AVERROR_INVALIDDATA;
	}
	if (player->has_audio && player->audio.first_packet_pts_valid) {
		uint64_t audio_pts_us =
		    (uint64_t)player->audio.first_packet_pts_ms * 1000ULL;
		if (audio_pts_us > requested_us +
		                   VITA_SW_DECODER_SEEK_OVERSHOOT_US) {
			log_printf("decoder seek: invalid audio landing target=%llu ms audio=%u ms overshoot=%llu ms; reopening\n",
			           (unsigned long long)position_ms,
			           player->audio.first_packet_pts_ms,
			           (unsigned long long)((audio_pts_us - requested_us) / 1000ULL));
			return AVERROR_INVALIDDATA;
		}
		uint64_t landing_delta_us = audio_pts_us > first_ready_pts_us
		                           ? audio_pts_us - first_ready_pts_us
		                           : first_ready_pts_us - audio_pts_us;
		if (landing_delta_us > VITA_SW_DECODER_AV_LANDING_TOLERANCE_US) {
			log_printf("decoder seek: A/V landing mismatch target=%llu ms video=%llu ms audio=%u ms delta=%llu ms; reopening\n",
			           (unsigned long long)position_ms,
			           (unsigned long long)(first_ready_pts_us / 1000ULL),
			           player->audio.first_packet_pts_ms,
			           (unsigned long long)(landing_delta_us / 1000ULL));
			return AVERROR_INVALIDDATA;
		}
	}
	player->start_gate = 1;
	uint64_t completed_us = sceKernelGetProcessTimeWide();
	log_printf("decoder seek: join=%llu ms video-demux=%llu ms audio-demux=%llu ms restart=%llu ms first-frame=%llu ms total=%llu ms ready=%d video=%llu ms audio=%u ms audio-valid=%d ret=0\n",
	           (unsigned long long)((joined_us - started_us) / 1000ULL),
	           (unsigned long long)((video_seek_done_us - joined_us) / 1000ULL),
	           (unsigned long long)((demux_done_us - video_seek_done_us) / 1000ULL),
	           (unsigned long long)((restarted_us - demux_done_us) / 1000ULL),
	           (unsigned long long)((completed_us - restarted_us) / 1000ULL),
	           (unsigned long long)((completed_us - started_us) / 1000ULL), ready,
	           (unsigned long long)(first_ready_pts_us / 1000ULL),
	           player->audio.first_packet_pts_ms,
	           player->has_audio ? player->audio.first_packet_pts_valid : 0);
	return 0;
}

static int open_session(VitaSwDecoderPlayer *player, uint64_t start_position_ms) {
	uint64_t started_us = sceKernelGetProcessTimeWide();
	int ret = 0;
	player->cancel = player->config.cancel_flag
	               ? player->config.cancel_flag : &player->local_cancel;
	if (player->config.cancel_flag) {
		/* Even a flag that was already raised must take the common failure path:
		 * it clears caller-owned pointer aliases and the unpublished GPU marker. */
		if (*player->cancel) {
			ret = AVERROR_EXIT;
			goto fail;
		}
	} else {
		player->local_cancel = 0;
	}
	player->start_gate = 0;
	player->clock_us = 0;
	player->clock_wall_us = 0;
	player->clock_started = 0;
	player->has_audio = 0;
	player->audio_clock_handed_off = 0;
	player->video_bitrate_bps = 0;
	memset(&player->video, 0, sizeof(player->video));
	memset(&player->audio, 0, sizeof(player->audio));

	ret = input_open(&player->video_input, &player->config.stream,
	                 player->cancel, "video", 1);
	if (ret < 0) goto fail;
	snapshot_tracks(player, player->video_input.format);
	int requested_audio_track = player->config.audio_track;
	if (player->audio_track_count <= 0 || player->config.audio_track < 0 ||
	    player->config.audio_track >= player->audio_track_count)
		player->config.audio_track = 0;
	log_printf("decoder tracks: audio=%d subtitles=%d requested=%d selected=%d\n",
	           player->audio_track_count, player->subtitle_track_count,
	           requested_audio_track, player->config.audio_track);
	int video_index = find_stream(player->video_input.format,
	                              AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264, 0);
	int audio_index = player->audio_track_count > 0
	                ? player->audio_tracks[player->config.audio_track].stream_index
	                : -1;
	if (video_index < 0) {
		ret = AVERROR_DECODER_NOT_FOUND;
		goto fail;
	}
	AVCodecParameters *video_parameters =
	    player->video_input.format->streams[video_index]->codecpar;
	if (video_parameters->bit_rate > 0)
		player->video_bitrate_bps = (uint64_t)video_parameters->bit_rate;
	else if (player->video_input.format->bit_rate > 0)
		player->video_bitrate_bps = (uint64_t)player->video_input.format->bit_rate;
	if (audio_index >= 0) {
		ret = input_open(&player->audio_input, &player->config.stream,
		                 player->cancel, "audio", 0);
		if (ret < 0) goto fail;
		if ((unsigned int)audio_index >= player->audio_input.format->nb_streams ||
		    !playable_aac_stream(
		        player->audio_input.format->streams[audio_index])) {
			ret = AVERROR_DECODER_NOT_FOUND;
			goto fail;
		}
		player->has_audio = 1;
	}
	isolate_input_stream(player->video_input.format, video_index);
	if (player->has_audio)
		isolate_input_stream(player->audio_input.format, audio_index);
	player->duration_ms = input_duration_ms(player->video_input.format);
	if (!player->duration_ms && player->has_audio)
		player->duration_ms = input_duration_ms(player->audio_input.format);
	if (!player->video_bitrate_bps && player->duration_ms > 0 &&
	    player->video_input.stream.size > 0) {
		uint64_t bytes = (uint64_t)player->video_input.stream.size;
		/* Matroska frequently omits per-stream and container bitrate metadata.
		 * Expose a useful average container rate instead of an empty HUD value. */
		player->video_bitrate_bps = bytes <= UINT64_MAX / 8000ULL
		                          ? bytes * 8000ULL / player->duration_ms
		                          : bytes / player->duration_ms * 8000ULL;
	}
	uint64_t video_end_ms = stream_logical_end_ms(player->video_input.format,
	                                             video_index);
	uint64_t playable_end_ms = video_end_ms &&
	                           (!player->duration_ms ||
	                            video_end_ms < player->duration_ms)
	                         ? video_end_ms : player->duration_ms;
	if (playable_end_ms && start_position_ms >= playable_end_ms) {
		log_printf("decoder startup: stale resume=%llu ms playable-end=%llu ms reset=0\n",
		           (unsigned long long)start_position_ms,
		           (unsigned long long)playable_end_ms);
		start_position_ms = 0;
	}

	if (start_position_ms > 0) {
		uint64_t seek_started_us = sceKernelGetProcessTimeWide();
		ret = seek_input_ms(&player->video_input, video_index, start_position_ms);
		uint64_t video_seek_done_us = sceKernelGetProcessTimeWide();
		if (ret >= 0 && player->has_audio)
			ret = seek_input_ms(&player->audio_input, audio_index, start_position_ms);
		uint64_t seek_done_us = sceKernelGetProcessTimeWide();
		log_printf("decoder startup seek: target=%llu ms video=%llu ms audio=%llu ms total=%llu ms ret=%d\n",
		           (unsigned long long)start_position_ms,
		           (unsigned long long)((video_seek_done_us - seek_started_us) / 1000ULL),
		           (unsigned long long)((seek_done_us - video_seek_done_us) / 1000ULL),
		           (unsigned long long)((seek_done_us - seek_started_us) / 1000ULL), ret);
		if (ret < 0) {
			/* A stale history point must never turn startup into a linear decode
			 * across a long file. Fall back to a checked beginning seek and report
			 * position zero to both pipelines. */
			int reset_ret = seek_input_ms(&player->video_input, video_index, 0);
			if (reset_ret >= 0 && player->has_audio)
				reset_ret = seek_input_ms(&player->audio_input, audio_index, 0);
			if (reset_ret < 0) {
				log_printf("decoder startup: resume=%llu ms reset-to-zero ret=%d\n",
				           (unsigned long long)start_position_ms, reset_ret);
				ret = reset_ret;
				goto fail;
			}
			start_position_ms = 0;
		}
	}
	/* Preserve the requested logical position even if the selected audio stream
	 * has already ended there. A clean audio EOF can then hand clock ownership to
	 * the wall clock without jumping back to zero. */
	player->clock_us = start_position_ms * 1000ULL;

	ret = vita_sw_software_video_start(&player->video, player->video_input.format,
	                           video_index, player->config.expected_width,
	                           player->config.expected_height,
	                           player->config.expected_fps, start_position_ms,
	                           player->cancel, &player->start_gate);
	if (ret < 0) goto fail;
	if (player->has_audio) {
		uint64_t audio_start_deadline = sceKernelGetProcessTimeWide() +
		                                VITA_SW_DECODER_AUDIO_READY_DEADLINE_US;
		ret = start_audio_candidate(player, audio_index, start_position_ms, NULL,
		                            audio_start_deadline);
		if (ret < 0) goto fail;
		vita_sw_elementary_audio_set_volume(&player->audio,
		                                    player->config.volume_percent);
		player->audio.paused = player->paused;
	}
	/* Never publish an audio-only session for a source that is supposed to contain
	 * H.264 video. CPU startup is allowed a longer bound than hardware, but it must
	 * either deliver a real picture or fail instead of leaving Preparing video on
	 * screen forever. */
	uint64_t gate_started_us = sceKernelGetProcessTimeWide();
	uint64_t deadline = gate_started_us + 6000000ULL;
	int ready = 0;
	while (!*player->cancel && !player->video.had_error && !player->video.eof &&
	       !(player->has_audio && player->audio.had_error) &&
	       sceKernelGetProcessTimeWide() < deadline) {
		vita_sw_software_video_buffer_status(&player->video, &ready, NULL, NULL);
		int audio_ready = !player->has_audio || player->audio.first_packet_ready ||
		                  player->audio.eof;
		if (ready >= 1 && audio_ready) break;
		sceKernelDelayThread(1000);
	}
	__sync_synchronize();
	uint64_t first_ready_pts_us = 0;
	vita_sw_software_video_buffer_status(&player->video, &ready, NULL,
	                                     &first_ready_pts_us);
	if (*player->cancel) { ret = AVERROR_EXIT; goto fail; }
	if (!ready && player->video.had_error) {
		ret = AVERROR_INVALIDDATA;
		log_printf("decoder startup: stage=first-frame ready=0 ret=%d cause=video-error\n",
		           ret);
		goto fail;
	}
	if (!ready && player->video.eof) {
		ret = AVERROR_EOF;
		log_printf("decoder startup: stage=first-frame ready=0 ret=%d cause=eof\n",
		           ret);
		goto fail;
	}
	if (!ready) {
		ret = AVERROR(ETIMEDOUT);
		log_printf("decoder startup: stage=first-frame ready=0 ret=%d cause=timeout\n",
		           ret);
		goto fail;
	}
	if (player->has_audio && !player->audio.first_packet_ready &&
	    !player->audio.eof) {
		ret = player->audio.had_error ? AVERROR_INVALIDDATA
		      : AVERROR(ETIMEDOUT);
		log_printf("decoder startup: stage=first-audio target=%llu ms ret=%d eof=%d error=%d\n",
		           (unsigned long long)start_position_ms, ret,
		           player->audio.eof, player->audio.had_error);
		goto fail;
	}
	uint64_t requested_us = start_position_ms * 1000ULL;
	if (first_ready_pts_us > requested_us +
	                         VITA_SW_DECODER_SEEK_OVERSHOOT_US) {
		ret = AVERROR_INVALIDDATA;
		log_printf("decoder startup: invalid video landing target=%llu ms video=%llu ms\n",
		           (unsigned long long)start_position_ms,
		           (unsigned long long)(first_ready_pts_us / 1000ULL));
		goto fail;
	}
	if (player->has_audio && player->audio.first_packet_pts_valid) {
		uint64_t audio_pts_us =
		    (uint64_t)player->audio.first_packet_pts_ms * 1000ULL;
		uint64_t landing_delta_us = audio_pts_us > first_ready_pts_us
		                           ? audio_pts_us - first_ready_pts_us
		                           : first_ready_pts_us - audio_pts_us;
		if (audio_pts_us > requested_us +
		                   VITA_SW_DECODER_SEEK_OVERSHOOT_US ||
		    landing_delta_us > VITA_SW_DECODER_AV_LANDING_TOLERANCE_US) {
			ret = AVERROR_INVALIDDATA;
			log_printf("decoder startup: invalid A/V landing target=%llu ms video=%llu ms audio=%u ms delta=%llu ms\n",
			           (unsigned long long)start_position_ms,
			           (unsigned long long)(first_ready_pts_us / 1000ULL),
			           player->audio.first_packet_pts_ms,
			           (unsigned long long)(landing_delta_us / 1000ULL));
			goto fail;
		}
	}
	player->start_gate = 1;
	player->opened = 1;
	uint64_t completed_us = sceKernelGetProcessTimeWide();
	log_printf("decoder startup: first-frame=%llu ms total=%llu ms ready=%d video=%llu ms audio=%u ms audio-valid=%d ret=0\n",
	           (unsigned long long)((completed_us - gate_started_us) / 1000ULL),
	           (unsigned long long)((completed_us - started_us) / 1000ULL), ready,
	           (unsigned long long)(first_ready_pts_us / 1000ULL),
	           player->audio.first_packet_pts_ms,
	           player->has_audio ? player->audio.first_packet_pts_valid : 0);
	return 0;

fail:
	log_printf("decoder startup: total=%llu ms ret=%d\n",
	           (unsigned long long)((sceKernelGetProcessTimeWide() - started_us) /
	                                1000ULL), ret);
	close_session(player);
	return ret;
}

VitaSwDecoderPlayer *vita_sw_decoder_create(void) {
	return calloc(1, sizeof(VitaSwDecoderPlayer));
}

int vita_sw_decoder_open(VitaSwDecoderPlayer *player,
	                    const VitaSwDecoderPlayerConfig *config) {
	if (!player || !config ||
	    (!config->stream.open && !config->stream.open_with_cancel))
		return AVERROR(EINVAL);
	close_session(player);
	player->config = *config;
	if (player->config.audio_track < 0) player->config.audio_track = 0;
	if (player->config.volume_percent < 0) player->config.volume_percent = 0;
	if (player->config.volume_percent > 300) player->config.volume_percent = 300;
	player->paused = 0;
	/* A newly opening session has never published a texture. Failure cleanup is
	 * therefore already GPU-safe and must not call vita2d from this worker. */
	player->video_gpu_fenced = 1;
	int ret = open_session(player, config->start_position_ms);
	if (ret >= 0) player->video_gpu_fenced = 0;
	return ret;
}

void vita_sw_decoder_close(VitaSwDecoderPlayer *player) {
	close_session(player);
}

void vita_sw_decoder_destroy(VitaSwDecoderPlayer *player) {
	if (!player) return;
	close_session(player);
	free(player);
}

void vita_sw_decoder_set_paused(VitaSwDecoderPlayer *player, int paused) {
	if (!player) return;
	player->paused = paused != 0;
	player->audio.paused = player->paused;
	player->clock_wall_us = sceKernelGetProcessTimeWide();
}

void vita_sw_decoder_set_volume(VitaSwDecoderPlayer *player, int percent) {
	if (!player) return;
	if (percent < 0) percent = 0;
	if (percent > 300) percent = 300;
	player->config.volume_percent = percent;
	vita_sw_elementary_audio_set_volume(&player->audio, percent);
}

void vita_sw_decoder_request_stop(VitaSwDecoderPlayer *player) {
	if (!player) return;
	if (player->cancel) *player->cancel = 1;
	player->video.stop = 1;
	player->audio.stop = 1;
	__sync_synchronize();
	input_cancel_and_abort(&player->video_input);
	input_cancel_and_abort(&player->audio_input);
}

void vita_sw_decoder_prepare_background_restart(VitaSwDecoderPlayer *player) {
	if (!player) return;
	/* The caller's GXM fence and presentation exclusion happen-before the
	 * restart worker observes this one-shot mark. */
	__sync_synchronize();
	player->video_gpu_fenced = 1;
	__sync_synchronize();
}

void vita_sw_decoder_interrupt_audio_operation(VitaSwDecoderPlayer *player) {
	if (!player) return;
	audio_operation_lock(player);
	/* The UI publishes its caller flag only after this phase-specific interrupt,
	 * so a forward cancellation cannot arrive late and poison rollback. A later
	 * user cancellation may still interrupt a genuinely blocked restore. */
	if (player->audio_operation_active) {
		player->audio_operation_interrupted = 1;
		input_cancel_and_abort(&player->audio_input);
	}
	__sync_synchronize();
	audio_operation_unlock(player);
}

int vita_sw_decoder_seek(VitaSwDecoderPlayer *player, uint64_t position_ms) {
	if (!player || !player->opened) return AVERROR(EINVAL);
	int video_index = find_stream(player->video_input.format,
	                              AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264, 0);
	uint64_t video_end_ms = stream_logical_end_ms(
	    player->video_input.format, video_index);
	uint64_t seek_end_ms = video_end_ms &&
	                       (!player->duration_ms ||
	                        video_end_ms < player->duration_ms)
	                     ? video_end_ms : player->duration_ms;
	if (seek_end_ms && position_ms >= seek_end_ms) {
		int fps = player->video.source_fps > 0 ? player->video.source_fps
		          : player->config.expected_fps > 0
		              ? player->config.expected_fps : 30;
		uint64_t tail_margin_ms = (2000ULL + (uint64_t)fps - 1ULL) /
		                          (uint64_t)fps;
		if (tail_margin_ms < 250ULL) tail_margin_ms = 250ULL;
		position_ms = seek_end_ms > tail_margin_ms
		            ? seek_end_ms - tail_margin_ms : 0;
		log_printf("decoder seek: end clamped duration=%llu ms video-end=%llu ms target=%llu ms margin=%llu ms\n",
		           (unsigned long long)player->duration_ms,
		           (unsigned long long)video_end_ms,
		           (unsigned long long)position_ms,
		           (unsigned long long)tail_margin_ms);
	}
	int paused = player->paused;
	uint64_t started_us = sceKernelGetProcessTimeWide();
	int ret = restart_session_in_place(player, position_ms);
	if (ret >= 0) {
		player->video_gpu_fenced = 0;
		__sync_synchronize();
		return 0;
	}
	if (ret == AVERROR_EXIT || (player->cancel && *player->cancel)) {
		player->video_gpu_fenced = 0;
		__sync_synchronize();
		log_printf("decoder seek recovery: skipped total=%llu ms ret=%d cancelled=1\n",
		           (unsigned long long)((sceKernelGetProcessTimeWide() - started_us) /
		                                1000ULL), ret);
		return AVERROR_EXIT;
	}
	volatile int *cancel_flag = player->config.cancel_flag;
	close_session(player);
	player->config.cancel_flag = cancel_flag;
	/* The recovery session is not yet visible to the renderer. Its failure path
	 * can release buffers without a worker-side vita2d fence. */
	player->video_gpu_fenced = 1;
	__sync_synchronize();
	player->paused = paused;
	int recovery_ret = open_session(player, position_ms);
	player->video_gpu_fenced = 0;
	__sync_synchronize();
	log_printf("decoder seek recovery: total=%llu ms initial-ret=%d recovery-ret=%d\n",
	           (unsigned long long)((sceKernelGetProcessTimeWide() - started_us) /
	                                1000ULL), ret, recovery_ret);
	return recovery_ret;
}

int vita_sw_decoder_select_audio_track_with_cancel(
	VitaSwDecoderPlayer *player, int audio_track, uint64_t position_ms,
	volatile int *operation_cancel) {
	if (!player || !player->opened || audio_track < 0 ||
	    audio_track >= player->audio_track_count) return AVERROR(EINVAL);
	if (audio_track == player->config.audio_track) return 0;
	if (operation_cancel && *operation_cancel) return AVERROR_EXIT;
	if (player->duration_ms && position_ms >= player->duration_ms) {
		int fps = player->video.source_fps > 0 ? player->video.source_fps
		          : player->config.expected_fps > 0
		              ? player->config.expected_fps : 30;
		uint64_t tail_margin_ms = (2000ULL + (uint64_t)fps - 1ULL) /
		                          (uint64_t)fps;
		if (tail_margin_ms < 250ULL) tail_margin_ms = 250ULL;
		position_ms = player->duration_ms > tail_margin_ms
		            ? player->duration_ms - tail_margin_ms : 0;
	}
	int selected_stream_index = player->audio_tracks[audio_track].stream_index;
	uint64_t selected_end_ms = indexed_container(player->video_input.format)
	    ? stream_logical_end_ms(player->video_input.format,
	                            selected_stream_index) : 0;
	if (selected_end_ms && position_ms >= selected_end_ms) {
		/* Reject before stopping the live worker. Stream duration is normalized to
		 * the common container origin, so a short or delayed replacement cannot be
		 * mistaken for having media at this logical position. */
		log_printf("decoder audio switch: reject track=%d position=%llu ms stream-end=%llu ms unchanged=%d\n",
		           audio_track, (unsigned long long)position_ms,
		           (unsigned long long)selected_end_ms,
		           VITA_SW_DECODER_AUDIO_CHANGE_ROLLED_BACK);
		return VITA_SW_DECODER_AUDIO_CHANGE_ROLLED_BACK;
	}
	int previous_track = player->config.audio_track;
	if (!audio_operation_begin(player)) return AVERROR(EBUSY);
	player->config.audio_track = audio_track;
	int ret = restart_audio_track(player, audio_track, position_ms,
	                              operation_cancel, 1);
	if (ret >= 0) {
		if (audio_operation_commit(player, operation_cancel)) return 0;
		ret = AVERROR_EXIT;
		abandon_audio_replacement(player, position_ms);
	}
	player->config.audio_track = previous_track;
	if (player->cancel && *player->cancel) {
		audio_operation_end(player, 0);
		return AVERROR_EXIT;
	}
	audio_operation_begin_rollback(player);
	int restore_ret = restart_audio_track(player, previous_track, position_ms,
	                                      &player->audio_operation_interrupted, 0);
	if (restore_ret >= 0) {
		if (audio_operation_commit(
		        player, &player->audio_operation_interrupted)) {
			log_printf("decoder audio rollback: prior track=%d restored forward-ret=%d",
			           previous_track, ret);
			return ret == AVERROR_EXIT
			     ? AVERROR_EXIT : VITA_SW_DECODER_AUDIO_CHANGE_ROLLED_BACK;
		}
		restore_ret = AVERROR_EXIT;
		abandon_audio_replacement(player, position_ms);
	}
	audio_operation_end(player, 0);
	/* AVERROR_EXIT is safe to treat as a dismissed forward change only when the
	 * prior track was committed again. Otherwise expose a real operation error
	 * so the UI cannot silently continue with has_audio == 0. */
	if (restore_ret == AVERROR_EXIT &&
	    !(player->cancel && *player->cancel))
		restore_ret = AVERROR(EIO);
	log_printf("decoder audio rollback: prior track=%d ret=%d live=0\n",
	           previous_track, restore_ret);
	return restore_ret;
}

int vita_sw_decoder_select_audio_track(VitaSwDecoderPlayer *player,
	                                   int audio_track,
	                                   uint64_t position_ms) {
	return vita_sw_decoder_select_audio_track_with_cancel(
	    player, audio_track, position_ms, NULL);
}

int vita_sw_decoder_audio_track_count(const VitaSwDecoderPlayer *player) {
	return player ? player->audio_track_count : 0;
}

int vita_sw_decoder_subtitle_track_count(const VitaSwDecoderPlayer *player) {
	return player ? player->subtitle_track_count : 0;
}

int vita_sw_decoder_audio_track_info(const VitaSwDecoderPlayer *player,
	                                 int index, VitaSwDecoderTrackInfo *info) {
	if (!player || !info || index < 0 || index >= player->audio_track_count)
		return AVERROR(EINVAL);
	*info = player->audio_tracks[index];
	return 0;
}

int vita_sw_decoder_subtitle_track_info(const VitaSwDecoderPlayer *player,
	                                    int index,
	                                    VitaSwDecoderTrackInfo *info) {
	if (!player || !info || index < 0 || index >= player->subtitle_track_count)
		return AVERROR(EINVAL);
	*info = player->subtitle_tracks[index];
	return 0;
}

static int audio_is_clock_master(const VitaSwDecoderPlayer *player) {
	int clean_eof = player->has_audio && player->audio.eof &&
	                !player->audio.had_error;
	/* The audio worker publishes its final accepted-sample clock before eof. Pair
	 * that release fence before handing the presentation clock to wall time. */
	if (clean_eof) __sync_synchronize();
	return player->has_audio && !clean_eof;
}

static uint64_t presentation_clock(VitaSwDecoderPlayer *player) {
	uint64_t now = sceKernelGetProcessTimeWide();
	if (!audio_is_clock_master(player)) {
		/* clock_us was continuously shadowing the audio clock before EOF. Keep the
		 * larger value at handoff, then advance from the same wall timestamp so the
		 * video tail neither freezes nor jumps backwards. */
		uint64_t raw = player->has_audio
		             ? vita_sw_elementary_audio_clock_us(&player->audio) : 0;
		if (raw > player->clock_us) player->clock_us = raw;
		if (player->has_audio && player->audio.eof &&
		    !player->audio.had_error && !player->audio_clock_handed_off) {
			player->audio_clock_handed_off = 1;
			player->clock_started = 1;
			player->clock_wall_us = now;
			return player->clock_us;
		}
		if (!player->clock_started) {
			player->clock_started = 1;
			player->clock_wall_us = now;
			return player->clock_us;
		}
		uint64_t delta = now - player->clock_wall_us;
		player->clock_wall_us = now;
		if (!player->paused) player->clock_us += delta;
		return player->clock_us;
	}
	uint64_t raw = vita_sw_elementary_audio_clock_us(&player->audio);
	if (!raw) return 0;
	if (!player->clock_started) {
		player->clock_started = 1;
		player->clock_us = raw;
		player->clock_wall_us = now;
		return raw;
	}
	uint64_t delta = now - player->clock_wall_us;
	player->clock_wall_us = now;
	if (!player->paused) player->clock_us += delta;
	uint64_t minimum = raw > VITA_SW_DECODER_CLOCK_WINDOW_US
	                 ? raw - VITA_SW_DECODER_CLOCK_WINDOW_US : 0;
	uint64_t maximum = raw + VITA_SW_DECODER_CLOCK_WINDOW_US;
	if (player->clock_us < minimum) player->clock_us = minimum;
	if (player->clock_us > maximum) player->clock_us = maximum;
	return player->clock_us;
}

int vita_sw_decoder_present(VitaSwDecoderPlayer *player, int fill_screen) {
	if (!player || !player->opened) return 0;
	uint64_t clock = presentation_clock(player);
	uint64_t raw = audio_is_clock_master(player)
	             ? vita_sw_elementary_audio_clock_us(&player->audio) : clock;
	return vita_sw_software_video_present(&player->video, clock, raw,
	                              fill_screen, !player->paused);
}

int vita_sw_decoder_present_rect(VitaSwDecoderPlayer *player,
	                            float x, float y, float width, float height,
	                            int fill_rect) {
	if (!player || !player->opened) return 0;
	uint64_t clock = presentation_clock(player);
	uint64_t raw = audio_is_clock_master(player)
	             ? vita_sw_elementary_audio_clock_us(&player->audio) : clock;
	return vita_sw_software_video_present_rect(&player->video,
	                                   clock, raw,
	                                   x, y, width, height, fill_rect,
	                                   !player->paused);
}

void vita_sw_decoder_discard_video_to_clock(VitaSwDecoderPlayer *player) {
	if (!player || !player->opened) return;
	uint64_t clock = presentation_clock(player);
	vita_sw_software_video_discard_to_clock(&player->video, clock);
}

void vita_sw_decoder_render_complete(VitaSwDecoderPlayer *player) {
	if (player && player->opened) vita_sw_software_video_render_complete(&player->video);
}

void vita_sw_decoder_get_status(VitaSwDecoderPlayer *player,
	                           VitaSwDecoderPlayerStatus *status) {
	if (!status) return;
	memset(status, 0, sizeof(*status));
	if (!player) return;
	SoftwareVideoDebugSnapshot debug;
	vita_sw_software_video_debug_snapshot(&player->video, &debug);
	int audio_master = audio_is_clock_master(player);
	uint64_t status_clock_us = audio_master
	                         ? vita_sw_elementary_audio_clock_us(&player->audio)
	                         : (player->opened ? presentation_clock(player)
	                                           : player->clock_us);
	status->opened = player->opened;
	status->paused = player->paused;
	status->eof = player->video.eof && debug.ready_frames == 0 &&
	              (!player->has_audio || player->audio.eof);
	status->error = player->video.had_error ||
	                (player->has_audio && player->audio.had_error);
	status->width = player->video.content_width;
	status->height = player->video.content_height;
	status->video_bitrate_bps = player->video_bitrate_bps;
	status->duration_ms = player->duration_ms;
	status->position_ms = status_clock_us / 1000ULL;
	status->hardware_accelerated = debug.hardware_accelerated;
	status->direct_rendering = debug.direct_rendering;
	status->ready_frames = debug.ready_frames + (debug.have_display ? 1 : 0);
	status->frame_capacity = debug.queue_capacity;
	status->fps = debug.source_fps;
	status->frames_decoded = debug.frames_decoded;
	status->frames_shown = debug.frames_shown;
	status->frames_dropped = debug.frames_dropped;
}
