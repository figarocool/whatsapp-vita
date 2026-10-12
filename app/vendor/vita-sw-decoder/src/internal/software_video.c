#include "internal/software_video.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <arm_neon.h>

#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <libavcodec/packet.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>

#include "decoder_runtime.h"
#include "module_log.h"

#define SCREEN_WIDTH  960
#define SCREEN_HEIGHT 544

#define SOFTWARE_VIDEO_THREAD_STACK    0x100000
#define SOFTWARE_VIDEO_WORKER_STACK    0x20000
#define SOFTWARE_VIDEO_DECODER_THREADS 3
#define SOFTWARE_VIDEO_THREAD_PRIORITY 65
#define SOFTWARE_VIDEO_POLL_US         1000
#define SOFTWARE_VIDEO_HIGHFPS_LATE_US 50000ULL
#define SOFTWARE_VIDEO_HIGHFPS_RECOVER_US 18000ULL
#define SOFTWARE_VIDEO_HIGHFPS_CATCHUP_US 65000ULL
#define SOFTWARE_VIDEO_HW_CATCHUP_ENTER_US 120000ULL
#define SOFTWARE_VIDEO_HW_CATCHUP_EXIT_US   35000ULL
#define SOFTWARE_VIDEO_HIGHFPS_RECOVER_HOLD_US (2ULL * 1000ULL * 1000ULL)
#define SOFTWARE_VIDEO_PACING_LOG_US (5ULL * 1000ULL * 1000ULL)
#define SOFTWARE_VIDEO_STARTUP_REPLAY_PACKETS 16
#define CDRAM_ALIGNMENT                0x40000u
#define ALIGN_UP_U32(v, a) (((v) + ((a) - 1u)) & ~((a) - 1u))

static int hardware_raster_supported(uint32_t width, uint32_t height) {
	return width > 0 && height > 0 && width <= 1920 && height <= 1088 &&
	       (uint64_t)width * (uint64_t)height <= 1280ULL * 720ULL;
}

/* The old CPU zero-copy experiment remains disabled: frame-threaded libavcodec
 * retains many user buffers and proved unsafe on Vita. h264_vita direct
 * rendering is a different, public path: the single hardware decoder writes
 * its output straight into our CDRAM-backed AVFrame pool. */
#ifndef SOFTWARE_VIDEO_ZERO_COPY_ENABLED
#define SOFTWARE_VIDEO_ZERO_COPY_ENABLED 0
#endif

#ifndef SOFTWARE_VIDEO_HARDWARE_DIRECT_ENABLED
#define SOFTWARE_VIDEO_HARDWARE_DIRECT_ENABLED 1
#endif

#ifndef VITA_SW_DECODER_HAS_H264_VITA
#define VITA_SW_DECODER_HAS_H264_VITA 0
#endif

enum {
	SLOT_FREE = 0,
	SLOT_WRITING,
	SLOT_READY,
	SLOT_DISPLAYING
};

/* PThread-Embedded's pthread_create(NULL attr) consults this weak override.
 * FFmpeg creates its four frame workers with NULL attributes; the VitaSDK
 * default is only 32 KiB, too narrow a safety margin for H.264. */
__attribute__((weak)) unsigned int _pthread_stack_default_user =
	SOFTWARE_VIDEO_WORKER_STACK;

static void slots_lock(SoftwareVideoState *st) {
	while (__sync_lock_test_and_set(&st->slot_lock, 1))
		sceKernelDelayThread(100);
}

static void slots_unlock(SoftwareVideoState *st) {
	__sync_lock_release(&st->slot_lock);
}

static int should_stop(const SoftwareVideoState *st) {
	return st->stop || (st->cancel && *st->cancel);
}

static int first_video_stream(const AVFormatContext *ctx) {
	for (unsigned int i = 0; ctx && i < ctx->nb_streams; i++) {
		if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
			return (int)i;
	}
	return -1;
}

static void *allocate_cdram(const char *name, uint32_t size, SceUID *uid) {
	SceKernelAllocMemBlockOpt opt;
	void *base = NULL;
	uint32_t allocation = ALIGN_UP_U32(size, CDRAM_ALIGNMENT);
	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
	opt.alignment = CDRAM_ALIGNMENT;
	SceUID memblock = sceKernelAllocMemBlock(
	    name, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, allocation, &opt);
	if (memblock < 0) return NULL;
	if (sceKernelGetMemBlockBase(memblock, &base) < 0 || !base) {
		sceKernelFreeMemBlock(memblock);
		return NULL;
	}
	if (sceGxmMapMemory(base, allocation,
	                    SCE_GXM_MEMORY_ATTRIB_READ |
	                    SCE_GXM_MEMORY_ATTRIB_WRITE) < 0) {
		sceKernelFreeMemBlock(memblock);
		return NULL;
	}
	memset(base, 0, allocation);
	*uid = memblock;
	return base;
}

static void free_cdram(SoftwareVideoFrameSlot *slot) {
	if (!slot || !slot->pixels) return;
	sceGxmUnmapMemory(slot->pixels);
	if (slot->memblock >= 0) sceKernelFreeMemBlock(slot->memblock);
	slot->pixels = NULL;
	slot->memblock = -1;
}

static void free_zero_surface(SoftwareVideoZeroSurface *surface) {
	if (!surface || !surface->pixels) return;
	sceGxmUnmapMemory(surface->pixels);
	if (surface->memblock >= 0) sceKernelFreeMemBlock(surface->memblock);
	surface->pixels = NULL;
	surface->memblock = -1;
	surface->in_use = 0;
}

static void zero_surface_release(void *opaque, uint8_t *data) {
	(void)data;
	SoftwareVideoZeroSurface *surface = (SoftwareVideoZeroSurface *)opaque;
	__sync_synchronize();
	surface->in_use = 0;
}

/* Each AVBufferRef owns one complete contiguous YUV surface. The callback is
 * the sole authority that returns it to the pool after decoder, ring and GPU
 * references have all been released. */
static int zero_copy_get_buffer2(AVCodecContext *decoder, AVFrame *frame,
	                             int flags) {
	(void)flags;
	SoftwareVideoState *st = (SoftwareVideoState *)decoder->opaque;
	if (!st || !st->zero_copy) return AVERROR(EINVAL);
	if (frame->format != AV_PIX_FMT_YUV420P &&
	    frame->format != AV_PIX_FMT_YUVJ420P
#if VITA_SW_DECODER_HAS_H264_VITA
	    && frame->format != AV_PIX_FMT_VITA_YUV420P
	    && frame->format != AV_PIX_FMT_VITA_NV12
#endif
	   )
		return AVERROR(EINVAL);
	if (frame->width <= 0 || frame->height <= 0 ||
	    (uint32_t)frame->width > st->texture_width ||
	    (uint32_t)frame->height > st->texture_height)
		return AVERROR(EINVAL);

	SoftwareVideoZeroSurface *surface = NULL;
	for (;;) {
		if (should_stop(st)) return AVERROR_EXIT;
		for (int i = 0; i < SOFTWARE_VIDEO_ZERO_COPY_SURFACES; i++) {
			if (__sync_bool_compare_and_swap(&st->zero_surfaces[i].in_use, 0, 1)) {
				surface = &st->zero_surfaces[i];
				break;
			}
		}
		if (surface) break;
		sceKernelDelayThread(SOFTWARE_VIDEO_POLL_US);
		__sync_fetch_and_add(&st->zero_surface_wait_us,
		                     SOFTWARE_VIDEO_POLL_US);
	}

	AVBufferRef *owner = av_buffer_create((uint8_t *)surface->pixels,
	                                      st->frame_bytes,
	                                      zero_surface_release, surface, 0);
	if (!owner) {
		surface->in_use = 0;
		return AVERROR(ENOMEM);
	}
	frame->buf[0] = owner;
	int layout_ret = av_image_fill_arrays(frame->data, frame->linesize,
	                                      (uint8_t *)surface->pixels,
	                                      (enum AVPixelFormat)frame->format,
	                                      (int)st->pitch,
	                                      (int)st->texture_height, 1);
	if (layout_ret < 0 || (uint32_t)layout_ret > st->frame_bytes) {
		av_buffer_unref(&frame->buf[0]);
		return layout_ret < 0 ? layout_ret : AVERROR(EINVAL);
	}
	frame->extended_data = frame->data;
	return 0;
}

static uint64_t scale_frame_timestamp_us(const SoftwareVideoState *st,
	                                     int64_t pts) {
	if (pts == AV_NOPTS_VALUE) return 0;
	int64_t scaled = av_rescale_q(pts, st->time_base,
	                              (AVRational){ 1, 1000000 });
	return scaled < 0 ? 0 : (uint64_t)scaled;
}

static uint64_t frame_pts_us(SoftwareVideoState *st, const AVFrame *frame) {
	/* h264_vita writes the timestamp returned by SceVideodec to frame->pts.
	 * FFmpeg then derives best_effort_timestamp from that PTS and the packet DTS.
	 * The packet currently leaving a frame-threaded decoder need not belong to the
	 * returned picture, so best_effort_timestamp can be several seconds ahead.
	 *
	 * More importantly, the public Vita backend returns bounded B-frame timestamp
	 * reordering: a future reference picture can be followed by three or four
	 * pictures whose PTS is lower. That is valid decode order, not a broken media
	 * clock. Preserve those raw PTS values and let the presentation queue order
	 * them. Only missing, duplicate, or implausibly large backward discontinuities
	 * are synthesized at the nominal cadence. */
	if (st->hardware_accelerated) {
		int raw_valid = frame->pts != AV_NOPTS_VALUE;
		uint64_t raw_us = raw_valid
		                ? scale_frame_timestamp_us(st, frame->pts) : 0;
		uint64_t interval_us = st->source_fps > 0
		                     ? (1000000ULL + (uint64_t)st->source_fps / 2ULL) /
		                       (uint64_t)st->source_fps
		                     : 33333ULL;
		uint64_t chosen_us = raw_us;
		int repaired = 0;
		int reordered = 0;
		uint64_t max_reorder_us = interval_us * 8ULL + 2000ULL;

		if (!st->hardware_pts_initialized) {
			if (!raw_valid) {
				int64_t fallback = frame->best_effort_timestamp;
				chosen_us = fallback != AV_NOPTS_VALUE
				          ? scale_frame_timestamp_us(st, fallback) : 0;
				st->hardware_pts_missing++;
				repaired = 1;
			}
			st->hardware_pts_initialized = 1;
		} else if (!raw_valid) {
			chosen_us = st->hardware_max_pts_us + interval_us;
			st->hardware_pts_missing++;
			repaired = 1;
		} else if (raw_us < st->hardware_max_pts_us &&
		           st->hardware_max_pts_us - raw_us <= max_reorder_us) {
			st->hardware_pts_nonmonotonic++;
			reordered = 1;
		} else if (raw_us <= st->hardware_max_pts_us) {
			chosen_us = st->hardware_max_pts_us + interval_us;
			st->hardware_pts_nonmonotonic++;
			repaired = 1;
		}

		if (reordered) {
			unsigned int count = st->hardware_pts_nonmonotonic;
			if (count <= 4 || (count & (count - 1u)) == 0) {
				log_printf("software video: HW PTS reorder #%u raw=%lld used=%llu maximum=%llu",
				           count, (long long)frame->pts,
				           (unsigned long long)chosen_us,
				           (unsigned long long)st->hardware_max_pts_us);
			}
		}
		if (repaired) {
			unsigned int repair = ++st->hardware_pts_repairs;
			/* Powers of two retain enough forensic detail without turning a bad
			 * timestamp run into synchronous per-frame storage writes. */
			if (repair <= 4 || (repair & (repair - 1u)) == 0) {
				log_printf("software video: repaired HW PTS #%u raw=%lld best=%lld used=%llu previous=%llu",
				           repair, (long long)frame->pts,
				           (long long)frame->best_effort_timestamp,
				           (unsigned long long)chosen_us,
				           (unsigned long long)st->hardware_max_pts_us);
			}
		}
		if (chosen_us > st->hardware_max_pts_us)
			st->hardware_max_pts_us = chosen_us;
		return chosen_us;
	}

	int64_t pts = frame->best_effort_timestamp;
	if (pts == AV_NOPTS_VALUE) pts = frame->pts;
	return scale_frame_timestamp_us(st, pts);
}

static int reserve_slot(SoftwareVideoState *st) {
	uint64_t wait_started = sceKernelGetProcessTimeWide();
	for (;;) {
		if (should_stop(st)) return -1;
		slots_lock(st);
		for (int i = 0; i < st->queue_capacity; i++) {
			if (st->slots[i].state == SLOT_FREE) {
				st->slots[i].state = SLOT_WRITING;
				slots_unlock(st);
				st->queue_wait_time_us +=
				    sceKernelGetProcessTimeWide() - wait_started;
				return i;
			}
		}
		slots_unlock(st);
		sceKernelDelayThread(SOFTWARE_VIDEO_POLL_US);
	}
}

static int copy_yuv420_frame(SoftwareVideoState *st, int slot_index,
	                         const AVFrame *frame) {
	if (frame->format != AV_PIX_FMT_YUV420P &&
	    frame->format != AV_PIX_FMT_YUVJ420P) {
		log_printf("software video: unsupported FFmpeg pixel format %d",
		           frame->format);
		return -1;
	}
	if (frame->width <= 0 || frame->height <= 0 ||
	    (uint32_t)frame->width > st->content_width ||
	    (uint32_t)frame->height > st->content_height) {
		log_printf("software video: unexpected frame size %dx%d (buffer %ux%u)",
		           frame->width, frame->height, st->content_width,
		           st->content_height);
		return -1;
	}

	uint8_t *base = (uint8_t *)st->slots[slot_index].pixels;
	uint8_t *dst_y = base;
	uint8_t *dst_uv = base + st->pitch * st->texture_height;
	int exact_width = (uint32_t)frame->width == st->pitch;
	/* 1280x720 is the hot path. Clearing the complete 1280x768 allocation
	 * before immediately overwriting 720 rows cost almost one extra megabyte
	 * of CPU writes per picture. Clear only the hidden aligned tail when no
	 * horizontal padding exists; smaller formats retain the simple full clear
	 * that also initializes their right-hand padding. */
	if (exact_width) {
		if (frame->linesize[0] == frame->width) {
			memcpy(dst_y, frame->data[0],
			       (size_t)frame->width * (size_t)frame->height);
		} else {
			for (int y = 0; y < frame->height; y++)
				memcpy(dst_y + (uint32_t)y * st->pitch,
				       frame->data[0] + y * frame->linesize[0], frame->width);
		}
		if ((uint32_t)frame->height < st->texture_height)
			memset(dst_y + (uint32_t)frame->height * st->pitch, 16,
			       st->pitch * (st->texture_height - (uint32_t)frame->height));
	} else {
		/* Black luma + neutral chroma also make the 360->368 padding harmless. */
		memset(dst_y, 16, st->pitch * st->texture_height);
		for (int y = 0; y < frame->height; y++)
			memcpy(dst_y + (uint32_t)y * st->pitch,
			       frame->data[0] + y * frame->linesize[0], frame->width);
	}

	int chroma_width = (frame->width + 1) / 2;
	int chroma_height = (frame->height + 1) / 2;
	if (!exact_width)
		memset(dst_uv, 128, st->pitch * (st->texture_height / 2));
	for (int y = 0; y < chroma_height; y++) {
		uint8_t *out = dst_uv + (uint32_t)y * st->pitch;
		const uint8_t *src_u = frame->data[1] + y * frame->linesize[1];
		const uint8_t *src_v = frame->data[2] + y * frame->linesize[2];
		int x = 0;
		/* Cortex-A9 NEON writes sixteen U/V pairs per instruction group instead
		 * of the scalar byte-at-a-time loop that dominated 720p copy time. */
		for (; x + 16 <= chroma_width; x += 16) {
			uint8x16x2_t uv;
			uv.val[0] = vld1q_u8(src_u + x);
			uv.val[1] = vld1q_u8(src_v + x);
			vst2q_u8(out + x * 2, uv);
		}
		for (; x < chroma_width; x++) {
			/* The working AvPlayer path feeds its NV12-like U,V plane to the
			 * GXM YVU swizzle. Writing V,U here swapped chroma a second time,
			 * producing blue skin and the cold cast seen on hardware. */
			out[x * 2] = src_u[x];
			out[x * 2 + 1] = src_v[x];
		}
	}
	if (exact_width && (uint32_t)chroma_height < st->texture_height / 2)
		memset(dst_uv + (uint32_t)chroma_height * st->pitch, 128,
		       st->pitch * (st->texture_height / 2 - (uint32_t)chroma_height));
	return 0;
}

static int publish_frame(SoftwareVideoState *st, const AVFrame *frame) {
	uint64_t pts_us = frame_pts_us(st, frame);
	/* observed_clock_us is written by the 32-bit ARM presentation thread.
	 * Keep it and the related 64-bit diagnostics under the existing ring lock
	 * so a read cannot combine halves from two different clock samples. */
	slots_lock(st);
	uint64_t clock_us = st->observed_clock_us;
	st->frames_decoded++;
	st->last_decoded_pts_us = pts_us;
	/* A backward demux seek starts at the preceding random-access picture. Feed
	 * that picture and its dependent AUs to H.264 so it can reconstruct the
	 * requested image, but keep decoded preroll out of the presentation ring.
	 * Dropping the compressed keyframe instead leaves the codec without its
	 * reference state and produces audio-only playback after a backward seek. */
	uint64_t requested_us = st->initial_position_ms * 1000ULL;
	if (requested_us > 0 && pts_us < requested_us) {
		st->frames_dropped++;
		slots_unlock(st);
		return 0;
	}
	if (requested_us > 0 && !st->initial_preroll_complete) {
		st->initial_preroll_complete = 1;
		log_printf("software video: seek preroll complete frame=%llu ms requested=%llu ms",
		           (unsigned long long)(pts_us / 1000ULL),
		           (unsigned long long)st->initial_position_ms);
	}
	/* If a reordered picture arrives after its presentation deadline has already
	 * passed, showing it would move the image backwards in time. This should be
	 * rare with the 16-slot look-ahead queue, but failing closed here avoids a
	 * visible reverse step during startup or after a temporary queue drain. */
	if (st->frames_shown > 0 && pts_us <= st->last_presented_pts_us) {
		st->frames_dropped++;
		st->frames_late_dropped++;
		slots_unlock(st);
		return 0;
	}
	slots_unlock(st);
	if (st->continuous_high_fps) {
		uint64_t late_us = clock_us > pts_us ? clock_us - pts_us : 0;
		uint64_t now_us = sceKernelGetProcessTimeWide();
		/* A modest, stable A/V offset is less disruptive than a long freeze. The
		 * old hardware policy entered catch-up at 75 ms and discarded every output
		 * picture; on a decoder running only slightly below 60 fps this hid 375
		 * consecutive frames before recovering. For a real delay, ask FFmpeg's H.264
		 * layer to skip non-reference pictures and keep publishing the remaining
		 * references. The visible video then advances faster toward audio instead
		 * of freezing while the codec still reconstructs every frame. */
		if (st->hardware_accelerated) {
			if (!st->catchup_drop_active &&
			    late_us >= SOFTWARE_VIDEO_HW_CATCHUP_ENTER_US) {
				st->catchup_drop_active = 1;
				st->decoder->skip_frame = AVDISCARD_NONREF;
				st->catchup_switches++;
				log_printf("software video: HW catch-up started, lag=%llu ms",
				           (unsigned long long)(late_us / 1000ULL));
			}
			if (st->catchup_drop_active) {
				if (late_us <= SOFTWARE_VIDEO_HW_CATCHUP_EXIT_US) {
					st->decoder->skip_frame = AVDISCARD_DEFAULT;
					st->catchup_drop_active = 0;
					st->catchup_switches++;
					log_printf("software video: HW catch-up completed, lag=%llu ms",
					           (unsigned long long)(late_us / 1000ULL));
				}
			}
		}
		/* Frame-threaded H.264 can fall behind while still delivering pictures
		 * in strict display order. The old per-frame hysteresis oscillated dozens
		 * of times per session (visible as uneven, wooden motion). Hold the reduced
		 * non-reference cadence until timing has stayed healthy for two complete
		 * seconds; an overloaded 60 fps stream therefore settles near a regular
		 * 30 fps instead of switching quality every few frames. */
		if (!st->hardware_accelerated && !st->catchup_nonref &&
		    late_us >= SOFTWARE_VIDEO_HIGHFPS_CATCHUP_US) {
			st->decoder->skip_frame = AVDISCARD_NONREF;
			st->catchup_nonref = 1;
			st->catchup_recover_started_us = 0;
			st->catchup_switches++;
		} else if (!st->hardware_accelerated && st->catchup_nonref) {
			if (late_us <= SOFTWARE_VIDEO_HIGHFPS_RECOVER_US) {
				if (!st->catchup_recover_started_us)
					st->catchup_recover_started_us = now_us;
				else if (now_us - st->catchup_recover_started_us >=
				         SOFTWARE_VIDEO_HIGHFPS_RECOVER_HOLD_US) {
					st->decoder->skip_frame = AVDISCARD_DEFAULT;
					st->catchup_nonref = 0;
					st->catchup_recover_started_us = 0;
					st->catchup_switches++;
				}
			} else {
				st->catchup_recover_started_us = 0;
			}
		}
		/* Do not spend another full-frame copy on a picture whose presentation
		 * deadline has already passed. The renderer keeps the last valid surface
		 * until a current picture is published. */
		if (!st->hardware_accelerated &&
		    late_us >= SOFTWARE_VIDEO_HIGHFPS_LATE_US) {
			__sync_fetch_and_add(&st->frames_dropped, 1);
			__sync_fetch_and_add(&st->frames_late_dropped, 1);
			return 0;
		}
	}
	int slot_index = reserve_slot(st);
	if (slot_index < 0) return AVERROR_EXIT;
	int publish_ret = 0;
	if (st->zero_copy) {
		SoftwareVideoFrameSlot *slot = &st->slots[slot_index];
		av_frame_unref(slot->frame);
		publish_ret = av_frame_ref(slot->frame, frame);
	} else {
		uint64_t copy_started = sceKernelGetProcessTimeWide();
		publish_ret = copy_yuv420_frame(st, slot_index, frame);
		st->copy_time_us += sceKernelGetProcessTimeWide() - copy_started;
	}
	if (publish_ret < 0) {
		slots_lock(st);
		st->slots[slot_index].state = SLOT_FREE;
		slots_unlock(st);
		return publish_ret;
	}

	slots_lock(st);
	SoftwareVideoFrameSlot *slot = &st->slots[slot_index];
	slot->pts_us = pts_us;
	slot->serial = ++st->next_serial;
	__sync_synchronize();
	slot->state = SLOT_READY;
	slots_unlock(st);
	return 0;
}

static int drain_frames(SoftwareVideoState *st, AVFrame *frame) {
	for (;;) {
		int ret = avcodec_receive_frame(st->decoder, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return ret;
		if (ret < 0) return ret;
		ret = publish_frame(st, frame);
		av_frame_unref(frame);
		if (ret < 0) return ret;
	}
}

static int flush_frames(SoftwareVideoState *st, AVFrame *frame) {
	/* send(NULL) may legally return EAGAIN when delayed output still has to
	 * be received. Drain that output and retry the flush packet instead of
	 * turning a normal B-frame tail into a playback failure. */
	int ret = AVERROR(EAGAIN);
	for (int attempt = 0; attempt < 8 && ret == AVERROR(EAGAIN); attempt++) {
		ret = avcodec_send_packet(st->decoder, NULL);
		if (ret == AVERROR(EAGAIN)) {
			int drain_ret = drain_frames(st, frame);
			if (drain_ret < 0 && drain_ret != AVERROR(EAGAIN) &&
			    drain_ret != AVERROR_EOF)
				return drain_ret;
		}
	}
	if (ret < 0 && ret != AVERROR_EOF) return ret;
	if (ret == AVERROR_EOF) return 0;
	ret = drain_frames(st, frame);
	return (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) ? 0 : ret;
}

static int read_video_packet(SoftwareVideoState *st, AVPacket *packet) {
	int ret;
	while ((ret = av_read_frame(st->demux, packet)) >= 0) {
		if (packet->stream_index == st->stream_index) {
			return 1;
		}
		av_packet_unref(packet);
	}
	return ret == AVERROR_EOF ? 0 : ret;
}

static void publish_thread_init(SoftwareVideoState *st, int result) {
	st->init_result = result;
	__sync_synchronize();
	st->init_done = 1;
}

static int open_h264_decoder(SoftwareVideoState *st, AVStream *stream,
	                         const AVCodec *codec, int hardware) {
	if (!codec) return AVERROR_DECODER_NOT_FOUND;
	AVCodecContext *decoder = avcodec_alloc_context3(codec);
	if (!decoder) return AVERROR(ENOMEM);
	int ret = avcodec_parameters_to_context(decoder, stream->codecpar);
	if (ret < 0) {
		avcodec_free_context(&decoder);
		return ret;
	}
	/* h264_vita converts packet timestamps through pkt_timebase. Leaving it at
	 * 0/1 made the codec assume that container ticks were already Vita ticks;
	 * it is especially visible at the denser 60 fps cadence. */
	decoder->pkt_timebase = stream->time_base;

	if (hardware) {
		/* h264_vita owns the complete SceVideodec lifecycle and is deliberately
		 * single-threaded at the FFmpeg layer: the asynchronous work happens in
		 * the Vita codec engine, not in CPU frame workers. Requesting ordinary
		 * h264_vita direct rendering is selected with its dedicated pixel format
		 * and a one-buffer NV12 allocator. If the CDRAM pool was unavailable,
		 * retain the proven copied YUV420 path in the same binary. */
		decoder->thread_count = 1;
		decoder->thread_type = 0;
		if (st->hardware_direct_available) {
#if VITA_SW_DECODER_HAS_H264_VITA
			/* Exact representation used by wiliwili's public GXM backend. */
			decoder->pix_fmt = AV_PIX_FMT_VITA_NV12;
			decoder->opaque = st;
			decoder->get_buffer2 = zero_copy_get_buffer2;
			st->zero_copy = 1;
			st->queue_capacity = SOFTWARE_VIDEO_FRAME_SLOTS;
#else
			decoder->pix_fmt = AV_PIX_FMT_YUV420P;
#endif
		} else {
			decoder->pix_fmt = AV_PIX_FMT_YUV420P;
		}
	} else {
		/* A hardware startup failure may arrive after direct-render buffers were
		 * prepared. CPU fallback always returns to the independent copied ring;
		 * it must never revive the old frame-threaded zero-copy experiment. */
		st->zero_copy = 0;
		st->queue_capacity = SOFTWARE_VIDEO_COPY_FRAME_SLOTS;
		decoder->thread_count = SOFTWARE_VIDEO_DECODER_THREADS;
		decoder->thread_type = FF_THREAD_FRAME;
		decoder->flags2 |= AV_CODEC_FLAG2_FAST;
		if (st->continuous_high_fps) {
			decoder->skip_frame = AVDISCARD_DEFAULT;
			decoder->skip_loop_filter = AVDISCARD_ALL;
		}
		if (SOFTWARE_VIDEO_ZERO_COPY_ENABLED && st->zero_copy) {
			decoder->opaque = st;
			decoder->get_buffer2 = zero_copy_get_buffer2;
		}
	}

	ret = avcodec_open2(decoder, codec, NULL);
	if (ret < 0) {
		if (hardware) st->zero_copy = 0;
		avcodec_free_context(&decoder);
		return ret;
	}
	st->decoder = decoder;
	st->hardware_accelerated = hardware;
	return 0;
}

static int decode_packet(SoftwareVideoState *st, AVPacket *packet,
	                     AVFrame *frame) {
	int ret = avcodec_send_packet(st->decoder, packet);
	if (ret == AVERROR(EAGAIN)) {
		ret = drain_frames(st, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret >= 0)
			ret = avcodec_send_packet(st->decoder, packet);
	}
	if (ret < 0) return ret;
	ret = drain_frames(st, frame);
	return (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? 0 : ret;
}

static void free_startup_packets(AVPacket **packets, int *count) {
	if (!packets || !count) return;
	for (int i = 0; i < *count; i++) av_packet_free(&packets[i]);
	*count = 0;
}

/* h264_vita postpones codec-engine initialization until SPS/PPS from the
 * first AUs are available. Keep only that bounded startup window so a clean
 * runtime rejection can reopen the untouched CPU codec and replay the same
 * packets without reopening the input stream. */
static int fallback_startup_to_cpu(SoftwareVideoState *st, AVStream *stream,
	                               AVPacket **packets, int count,
	                               AVFrame *frame, int hardware_error) {
	if (!st->hardware_accelerated || st->frames_decoded != 0 || count <= 0)
		return hardware_error;
	log_printf("software video: h264_vita runtime -> %d before first frame; replaying %d AUs on CPU",
	           hardware_error, count);
	av_frame_unref(frame);
	avcodec_free_context(&st->decoder);
	const AVCodec *software_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	int ret = open_h264_decoder(st, stream, software_codec, 0);
	if (ret < 0) return ret;
	for (int i = 0; i < count && !should_stop(st); i++) {
		ret = decode_packet(st, packets[i], frame);
		if (ret < 0) return ret;
	}
	return should_stop(st) ? AVERROR_EXIT : 0;
}

/* FFmpeg's frame-threaded H.264 decoder uses pthread condition variables.
 * Its complete lifetime therefore has to belong to a pthread-created thread:
 * a raw sceKernelCreateThread has no VitaSDK pthread metadata and crashes in
 * pte_osSemaphoreCancellablePend as soon as avcodec_send_packet() wakes the
 * frame workers. avcodec_open2/free_context live here too because they create
 * and join those workers. */
static void *vita_sw_software_video_thread(void *argp) {
	SoftwareVideoState *st = (SoftwareVideoState *)argp;
	AVPacket *packet = NULL;
	AVFrame *frame = NULL;
	AVPacket *startup_packets[SOFTWARE_VIDEO_STARTUP_REPLAY_PACKETS] = {0};
	int startup_packet_count = 0;
	int startup_replay_available = 1;
	int init_ret = 0;
	int priority_before = sceKernelGetThreadCurrentPriority();
	int priority_ret = sceKernelChangeThreadPriority(sceKernelGetThreadId(),
	                                                SOFTWARE_VIDEO_THREAD_PRIORITY);
	log_printf("software video: thread priority %d -> %d ret=0x%08X",
	           priority_before, sceKernelGetThreadCurrentPriority(),
	           (unsigned)priority_ret);

	if (should_stop(st)) {
		init_ret = AVERROR_EXIT;
	} else {
		AVStream *stream = st->demux->streams[st->stream_index];
		const AVCodec *hardware_codec = NULL;
#if VITA_SW_DECODER_HAS_H264_VITA
		if (st->hardware_runtime_ready)
			hardware_codec = avcodec_find_decoder_by_name("h264_vita");
#endif
		if (hardware_codec) {
			init_ret = open_h264_decoder(st, stream, hardware_codec, 1);
			if (init_ret < 0) {
				log_printf("software video: h264_vita open -> %d; using CPU fallback",
				           init_ret);
		}
		}
		if (!hardware_codec || init_ret < 0) {
			const AVCodec *software_codec =
			    avcodec_find_decoder(AV_CODEC_ID_H264);
			init_ret = open_h264_decoder(st, stream, software_codec, 0);
		}
	}
	if (init_ret >= 0) {
		packet = av_packet_alloc();
		frame = av_frame_alloc();
		if (!packet || !frame) init_ret = AVERROR(ENOMEM);
	}
	if (init_ret < 0) {
		if (!should_stop(st)) {
			log_printf("software video: pthread/decoder initialization -> %d",
			           init_ret);
			st->had_error = 1;
		}
		publish_thread_init(st, init_ret);
		goto done;
	}

	publish_thread_init(st, 0);

	while (!should_stop(st)) {
		int read_ret = read_video_packet(st, packet);
		if (read_ret == 0) {
			int flush_ret = flush_frames(st, frame);
			if (flush_ret < 0 && !should_stop(st)) {
				log_printf("software video: final flush -> %d", flush_ret);
				st->had_error = 1;
			}
			st->eof = !st->had_error;
			break;
		}
		if (read_ret < 0) {
			if (!should_stop(st)) {
				log_printf("software video: av_read_frame -> %d", read_ret);
				st->had_error = 1;
			}
			break;
		}

		st->packets_read++;
		if (st->hardware_accelerated && st->frames_decoded == 0 &&
		    startup_replay_available) {
			if (startup_packet_count < SOFTWARE_VIDEO_STARTUP_REPLAY_PACKETS) {
				startup_packets[startup_packet_count] = av_packet_clone(packet);
				if (startup_packets[startup_packet_count])
					startup_packet_count++;
				else
					startup_replay_available = 0;
			} else {
				startup_replay_available = 0;
			}
		}
		uint64_t t0 = sceKernelGetProcessTimeWide();
		int ret = decode_packet(st, packet, frame);
		if (ret < 0 && startup_replay_available)
			ret = fallback_startup_to_cpu(st,
			                              st->demux->streams[st->stream_index],
			                              startup_packets, startup_packet_count,
			                              frame, ret);
		av_packet_unref(packet);
		if (st->frames_decoded > 0 || !startup_replay_available)
			free_startup_packets(startup_packets, &startup_packet_count);
		if (ret < 0) {
			if (!should_stop(st)) {
				log_printf("software video: decode packet -> %d", ret);
				st->had_error = 1;
			}
			break;
		}
		st->decode_time_us += sceKernelGetProcessTimeWide() - t0;
	}


done:
	free_startup_packets(startup_packets, &startup_packet_count);
	av_packet_free(&packet);
	av_frame_free(&frame);
	avcodec_free_context(&st->decoder);
	log_printf("software video: thread end pkt=%u frame=%u decode=%llu ms copy=%llu ms queue_wait=%llu ms surface_wait=%u ms late_drop=%u catchup_switch=%u pts_fix=%u missing=%u nonmono=%u zero=%d eof=%d err=%d",
	           st->packets_read, st->frames_decoded,
	           (unsigned long long)(st->decode_time_us / 1000ULL),
	           (unsigned long long)(st->copy_time_us / 1000ULL),
	           (unsigned long long)(st->queue_wait_time_us / 1000ULL),
	           st->zero_surface_wait_us / 1000u, st->frames_late_dropped,
	           st->catchup_switches, st->hardware_pts_repairs,
	           st->hardware_pts_missing, st->hardware_pts_nonmonotonic,
	           st->zero_copy,
	           st->eof, st->had_error);
	__sync_synchronize();
	st->thread_done = 1;
	return NULL;
}

static void release_frame_buffers(SoftwareVideoState *st) {
	if (!st) return;
	for (int i = 0; i < SOFTWARE_VIDEO_FRAME_SLOTS; i++) {
		if (st->slots[i].frame) {
			av_frame_free(&st->slots[i].frame);
		}
		free_cdram(&st->slots[i]);
	}
	for (int i = 0; i < SOFTWARE_VIDEO_ZERO_COPY_SURFACES; i++)
		free_zero_surface(&st->zero_surfaces[i]);
}

static int allocate_copy_ring(SoftwareVideoState *st) {
	for (int i = 0; i < SOFTWARE_VIDEO_COPY_FRAME_SLOTS; i++) {
		char name[32];
		snprintf(name, sizeof(name), "VitaSwDecoderSwFrame%d", i);
		st->slots[i].allocation_size = ALIGN_UP_U32(st->frame_bytes,
		                                                CDRAM_ALIGNMENT);
		st->slots[i].pixels = allocate_cdram(name, st->frame_bytes,
		                                           &st->slots[i].memblock);
		if (!st->slots[i].pixels) return AVERROR(ENOMEM);
	}
	return 0;
}

static __attribute__((unused)) int allocate_zero_copy_pool(SoftwareVideoState *st) {
	for (int i = 0; i < SOFTWARE_VIDEO_FRAME_SLOTS; i++) {
		st->slots[i].frame = av_frame_alloc();
		if (!st->slots[i].frame) return AVERROR(ENOMEM);
	}
	for (int i = 0; i < SOFTWARE_VIDEO_ZERO_COPY_SURFACES; i++) {
		char name[32];
		snprintf(name, sizeof(name), "VitaSwDecoderZeroYuv%d", i);
		SoftwareVideoZeroSurface *surface = &st->zero_surfaces[i];
		surface->allocation_size = ALIGN_UP_U32(st->frame_bytes,
		                                            CDRAM_ALIGNMENT);
		surface->pixels = allocate_cdram(name, st->frame_bytes,
		                                       &surface->memblock);
		if (!surface->pixels) return AVERROR(ENOMEM);
	}
	/* Fail closed before FFmpeg owns a surface. Texture initialization is
	 * metadata-only and proves that this firmware accepts direct NV12/P2. */
	int tex_ret = sceGxmTextureInitLinear(
	    &st->texture.gxm_tex, st->zero_surfaces[0].pixels,
	    SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
	    st->texture_width, st->texture_height, 0);
	if (tex_ret < 0) return tex_ret;
	return 0;
}

int vita_sw_software_video_start(SoftwareVideoState *st, AVFormatContext *demux,
	                     int stream_index, uint32_t expected_width,
	                     uint32_t expected_height, int expected_fps,
	                     uint64_t initial_position_ms,
	                     volatile int *cancel,
	                     volatile int *start_gate) {
	if (!st || !demux || !cancel) return AVERROR(EINVAL);
	memset(st, 0, sizeof(*st));
	st->demux = demux;
	st->cancel = cancel;
	st->start_gate = start_gate;
	st->display_slot = -1;
	st->retire_slot = -1;
	st->queue_capacity = SOFTWARE_VIDEO_COPY_FRAME_SLOTS;
	for (int i = 0; i < SOFTWARE_VIDEO_FRAME_SLOTS; i++)
		st->slots[i].memblock = -1;
	for (int i = 0; i < SOFTWARE_VIDEO_ZERO_COPY_SURFACES; i++)
		st->zero_surfaces[i].memblock = -1;

	if (stream_index < 0) stream_index = first_video_stream(demux);
	if (stream_index < 0 || (unsigned int)stream_index >= demux->nb_streams)
		return AVERROR_STREAM_NOT_FOUND;
	AVStream *stream = demux->streams[stream_index];
	if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
	    stream->codecpar->codec_id != AV_CODEC_ID_H264) {
		log_printf("software video: codec is not H.264 id=%d",
		           stream->codecpar->codec_id);
		return AVERROR_DECODER_NOT_FOUND;
	}
	st->stream_index = stream_index;
	st->time_base = stream->time_base;
	uint32_t width = stream->codecpar->width > 0
	               ? (uint32_t)stream->codecpar->width : expected_width;
	uint32_t height = stream->codecpar->height > 0
	                ? (uint32_t)stream->codecpar->height : expected_height;
	if (!hardware_raster_supported(width, height)) {
		log_printf("software video: dimensions exceed budget %ux%u", width, height);
		return AVERROR(EINVAL);
	}
	st->content_width = width;
	st->content_height = height;
	st->source_fps = expected_fps;
	st->initial_position_ms = initial_position_ms;
	st->initial_preroll_complete = initial_position_ms == 0;
	if (st->source_fps <= 0 && stream->avg_frame_rate.num > 0 &&
	    stream->avg_frame_rate.den > 0) {
		st->source_fps = (stream->avg_frame_rate.num +
		                  stream->avg_frame_rate.den / 2) /
		                 stream->avg_frame_rate.den;
	}
	st->continuous_high_fps = st->source_fps >= 50 &&
	                          (uint64_t)width * height >= 1280ULL * 640ULL;
#if VITA_SW_DECODER_HAS_H264_VITA
	/* ReAvPlayer is already a process-lifetime dependency of the working 360p
	 * path. Load the shared codec runtime before the h264_vita thread, while
	 * leaving h264_vita itself in sole ownership of the SceVideodec lifecycle. */
	int hardware_prepare_ret = vita_sw_decoder_runtime_prepare();
	if (hardware_prepare_ret >= 0) {
		st->hardware_runtime_ready = 1;
	} else {
		log_printf("software video: Vita H.264 runtime unavailable -> 0x%08X; using CPU",
		           (unsigned)hardware_prepare_ret);
	}
#endif
	/* wiliwili's public h264_vita direct path requests VITA_NV12, whose decoder
	 * pitch is 16-byte aligned. Match it exactly for cropped widths such as 854;
	 * it is also sufficient for the NEON copy fallback. */
	st->texture_width = ALIGN_UP_U32(width, 16);
	/* h264_vita's public direct-render contract lays the UV plane after the
	 * 16-row-aligned decoded height. Keep the GXM texture on that exact layout;
	 * the CPU copy fallback writes only visible rows and does not need to mirror
	 * libavcodec's private SIMD allocation envelope. */
	st->texture_height = ALIGN_UP_U32(height, 16);
	st->pitch = st->texture_width;
	st->frame_bytes = st->pitch * st->texture_height * 3 / 2;

	/* Keep the copied ring allocated even when hardware direct rendering is
	 * available. It is the immediate, in-process landing zone if h264_vita
	 * rejects the first SPS/AU and the startup packets are replayed on CPU. */
	int buffer_ret = allocate_copy_ring(st);
	if (buffer_ret < 0) {
		log_printf("software video: P2 ring allocation failed -> %d",
		           buffer_ret);
		release_frame_buffers(st);
		return buffer_ret;
	}
	st->ring_memory_bytes = st->slots[0].allocation_size *
	                       SOFTWARE_VIDEO_COPY_FRAME_SLOTS;
#if VITA_SW_DECODER_HAS_H264_VITA
	if (SOFTWARE_VIDEO_HARDWARE_DIRECT_ENABLED && st->hardware_runtime_ready) {
		buffer_ret = allocate_zero_copy_pool(st);
		if (buffer_ret >= 0) {
			st->hardware_direct_available = 1;
			st->ring_memory_bytes += st->zero_surfaces[0].allocation_size *
			                         SOFTWARE_VIDEO_ZERO_COPY_SURFACES;
		} else {
			/* Partial pool allocations cannot be used, but the copied ring is
			 * independent and remains valid. */
			log_printf("software video: direct CDRAM rendering unavailable -> 0x%08X; using copied output",
			           (unsigned)buffer_ret);
			for (int i = 0; i < SOFTWARE_VIDEO_FRAME_SLOTS; i++)
				av_frame_free(&st->slots[i].frame);
			for (int i = 0; i < SOFTWARE_VIDEO_ZERO_COPY_SURFACES; i++)
				free_zero_surface(&st->zero_surfaces[i]);
		}
	}
#endif

	pthread_attr_t attr;
	int ret = pthread_attr_init(&attr);
	int attr_ready = ret == 0;
	if (ret == 0)
		ret = pthread_attr_setstacksize(&attr, SOFTWARE_VIDEO_THREAD_STACK);
	if (ret == 0)
		ret = pthread_create(&st->thread, &attr, vita_sw_software_video_thread, st);
	if (attr_ready) pthread_attr_destroy(&attr);
	if (ret != 0) {
		log_printf("software video: pthread_create -> %d", ret);
		release_frame_buffers(st);
		return AVERROR(ret);
	}
	st->started = 1;
	/* The setup API remains synchronous: audio must not be started until the
	 * decoder has either opened successfully or reported its exact error. */
	while (!st->init_done) {
		if (*cancel) st->stop = 1;
		sceKernelDelayThread(SOFTWARE_VIDEO_POLL_US);
	}
	__sync_synchronize();
	if (st->init_result < 0 || *cancel) {
		int join_ret = pthread_join(st->thread, NULL);
		if (join_ret != 0) {
			log_printf("software video: pthread_join after init -> %d", join_ret);
			while (!st->thread_done)
				sceKernelDelayThread(SOFTWARE_VIDEO_POLL_US);
			__sync_synchronize();
		}
		ret = st->init_result < 0 ? st->init_result : AVERROR_EXIT;
		release_frame_buffers(st);
		st->started = 0;
		return ret;
	}
	log_printf("software video: H.264 ready backend=%s %ux%u %dfps texture=%ux%u pitch=%u memory=%u KiB queue=%d mode=%s thread=%d active=0x%X highfps=%d",
	           st->hardware_accelerated ? "H264_VITA_HW" : "FFMPEG_CPU",
	           width, height, st->source_fps, st->texture_width, st->texture_height,
	           st->pitch,
	           st->ring_memory_bytes / 1024u,
	           st->queue_capacity,
	           st->zero_copy ? "HW_DIRECT_NV12" : "COPY_YUV420P2",
	           SOFTWARE_VIDEO_DECODER_THREADS,
	           st->decoder ? st->decoder->active_thread_type : 0,
	           st->continuous_high_fps);
	return 0;
}

static void draw_frame_fit(const vita2d_texture *texture,
	                       uint32_t texture_width, uint32_t texture_height,
	                       uint32_t content_width, uint32_t content_height,
	                       float target_x, float target_y,
	                       float target_width, float target_height,
	                       int fill_target) {
	float sx = target_width / (float)content_width;
	float sy = target_height / (float)content_height;
	float scale = fill_target ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
	float x = target_x + (target_width - content_width * scale) * 0.5f;
	float y = target_y + (target_height - content_height * scale) * 0.5f;
	if (texture_width == content_width && texture_height == content_height)
		vita2d_draw_texture_scale(texture, x, y, scale, scale);
	else
		vita2d_draw_texture_part_scale(texture, x, y, 0, 0,
		                               content_width, content_height,
		                               scale, scale);
}

int vita_sw_software_video_present_rect(SoftwareVideoState *st,
	                            uint64_t presentation_clock_us,
	                            uint64_t raw_audio_clock_us,
	                            float x, float y, float width, float height,
	                            int fill_rect, int allow_advance) {
	if (!st) return 0;
	if (width <= 0.0f || height <= 0.0f) return 0;
	uint64_t audio_clock_us = presentation_clock_us;
	int selected = -1;
	int newest_due = -1;
	uint32_t selected_serial = 0;
	uint32_t newest_serial = 0;
	uint64_t oldest_due_pts_us = 0;
	uint64_t newest_due_pts_us = 0;
	uint64_t selected_pts_us = 0;
	uint64_t selected_wall_us = 0;
	slots_lock(st);
	st->observed_clock_us = audio_clock_us;
	/* h264_vita may publish future reference pictures before the B-frames that
	 * precede them in display time. Select by PTS, using serial only to break an
	 * exact tie. At 60 fps, keep the oldest due picture while it is recoverable;
	 * only jump to the newest due PTS after a real >40 ms delay. */
	for (int i = 0; allow_advance && i < st->queue_capacity; i++) {
		SoftwareVideoFrameSlot *slot = &st->slots[i];
		if (slot->state != SLOT_READY || slot->pts_us > audio_clock_us) continue;
		if (st->frames_shown > 0 && slot->pts_us <= st->last_presented_pts_us) {
			slot->state = SLOT_FREE;
			if (st->zero_copy && slot->frame) av_frame_unref(slot->frame);
			st->frames_dropped++;
			st->frames_late_dropped++;
			continue;
		}
		if (selected < 0 || slot->pts_us < oldest_due_pts_us ||
		    (slot->pts_us == oldest_due_pts_us && slot->serial < selected_serial)) {
			selected = i;
			selected_serial = slot->serial;
			oldest_due_pts_us = slot->pts_us;
		}
		if (newest_due < 0 || slot->pts_us > newest_due_pts_us ||
		    (slot->pts_us == newest_due_pts_us && slot->serial > newest_serial)) {
			newest_due = i;
			newest_serial = slot->serial;
			newest_due_pts_us = slot->pts_us;
		}
	}
	if (st->continuous_high_fps && selected >= 0 && newest_due >= 0 &&
	    audio_clock_us > st->slots[selected].pts_us + 40000ULL) {
		selected = newest_due;
		selected_serial = newest_serial;
		oldest_due_pts_us = newest_due_pts_us;
	} else if (!st->continuous_high_fps && newest_due >= 0) {
		selected = newest_due;
		selected_serial = newest_serial;
		oldest_due_pts_us = newest_due_pts_us;
	}
	if (selected >= 0) {
		for (int i = 0; i < st->queue_capacity; i++) {
			SoftwareVideoFrameSlot *slot = &st->slots[i];
			if (i != selected && slot->state == SLOT_READY &&
			    slot->pts_us <= audio_clock_us &&
			    (slot->pts_us < oldest_due_pts_us ||
			     (slot->pts_us == oldest_due_pts_us &&
			      slot->serial < selected_serial))) {
				slot->state = SLOT_FREE;
				if (st->zero_copy && slot->frame)
					av_frame_unref(slot->frame);
				st->frames_dropped++;
			}
		}
		if (st->display_slot >= 0) {
			/* render_complete() runs every frame; a second retirement here
			 * would indicate a caller contract violation, so keep it owned. */
			if (st->retire_slot < 0) st->retire_slot = st->display_slot;
		}
		st->display_slot = selected;
		st->slots[selected].state = SLOT_DISPLAYING;
		selected_pts_us = st->slots[selected].pts_us;
		st->last_presented_pts_us = selected_pts_us;
		st->frames_shown++;
		selected_wall_us = sceKernelGetProcessTimeWide();
		if (st->last_presented_wall_us) {
			uint64_t gap_us = selected_wall_us - st->last_presented_wall_us;
			if (gap_us > UINT32_MAX) gap_us = UINT32_MAX;
			if (gap_us > st->presentation_gap_max_us)
				st->presentation_gap_max_us = (uint32_t)gap_us;
			uint64_t nominal_us = st->source_fps > 0
			                    ? 1000000ULL / (uint64_t)st->source_fps : 0;
			if (nominal_us && gap_us > nominal_us + nominal_us / 2ULL)
				st->presentation_cadence_breaks++;
		}
		st->last_presented_wall_us = selected_wall_us;
	}
	int display = st->display_slot;
	slots_unlock(st);

	if (display < 0) return 0;
	/* Keep the same diagnostic for 24/30/60 fps. This is essential when the
	 * catalog response advertised 60 fps but the playback response selected a
	 * different representation: the log then proves both the nominal cadence
	 * that actually reached the decoder and its measured media/wall rate. */
	if (selected >= 0) {
		uint64_t now_us = selected_wall_us;
		if (!st->pacing_diag_wall_us) {
			st->pacing_diag_wall_us = now_us;
			st->pacing_diag_pts_us = selected_pts_us;
			st->pacing_diag_frames_shown = st->frames_shown;
		} else if (now_us - st->pacing_diag_wall_us >=
		           SOFTWARE_VIDEO_PACING_LOG_US) {
			uint64_t wall_span = now_us - st->pacing_diag_wall_us;
			uint64_t media_span = selected_pts_us >= st->pacing_diag_pts_us
			                    ? selected_pts_us - st->pacing_diag_pts_us : 0;
			unsigned int shown = st->frames_shown - st->pacing_diag_frames_shown;
			unsigned int cadence_x10 = wall_span
			    ? (unsigned int)((uint64_t)shown * 10000000ULL / wall_span) : 0;
			unsigned int media_rate_permille = wall_span
			    ? (unsigned int)(media_span * 1000ULL / wall_span) : 0;
			uint64_t lag_us = audio_clock_us > selected_pts_us
			                ? audio_clock_us - selected_pts_us : 0;
			int64_t av_drift_us = (int64_t)presentation_clock_us -
			                      (int64_t)raw_audio_clock_us;
			log_printf("software video pacing: nominal=%d fps shown=%u.%u fps media/wall=%u.%03ux lag=%llu ms av=%+lld ms decoded=%u shown_total=%u drop=%u late=%u catchup=%d pts_fix=%u cadence_break=%u gap_max=%u ms",
			           st->source_fps, cadence_x10 / 10, cadence_x10 % 10,
			           media_rate_permille / 1000, media_rate_permille % 1000,
			           (unsigned long long)(lag_us / 1000ULL),
			           (long long)(av_drift_us / 1000LL),
			           st->frames_decoded, st->frames_shown, st->frames_dropped,
			           st->frames_late_dropped,
			           st->catchup_nonref || st->catchup_drop_active,
			           st->hardware_pts_repairs,
			           st->presentation_cadence_breaks,
			           st->presentation_gap_max_us / 1000U);
			st->pacing_diag_wall_us = now_us;
			st->pacing_diag_pts_us = selected_pts_us;
			st->pacing_diag_frames_shown = st->frames_shown;
		}
	}
	void *texture_pixels = st->zero_copy
	                     ? (void *)st->slots[display].frame->data[0]
	                     : st->slots[display].pixels;
	int tex_ret = sceGxmTextureInitLinear(
	    &st->texture.gxm_tex, texture_pixels,
	    st->zero_copy ? SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1
	                  : SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
	    st->texture_width, st->texture_height, 0);
	if (tex_ret < 0) {
		log_printf("software video: sceGxmTextureInitLinear -> 0x%08X",
		           (unsigned)tex_ret);
		st->had_error = 1;
		return 0;
	}
	vita2d_texture_set_filters(&st->texture, SCE_GXM_TEXTURE_FILTER_LINEAR,
	                           SCE_GXM_TEXTURE_FILTER_LINEAR);
	draw_frame_fit(&st->texture, st->texture_width, st->texture_height,
	               st->content_width, st->content_height,
	               x, y, width, height, fill_rect);
	return display >= 0;
}

int vita_sw_software_video_present(SoftwareVideoState *st,
	                       uint64_t presentation_clock_us,
	                       uint64_t raw_audio_clock_us,
	                       int fill_screen, int allow_advance) {
	return vita_sw_software_video_present_rect(
	    st, presentation_clock_us, raw_audio_clock_us,
	    0.0f, 0.0f, (float)SCREEN_WIDTH, (float)SCREEN_HEIGHT,
	    fill_screen, allow_advance);
}

void vita_sw_software_video_discard_to_clock(SoftwareVideoState *st,
	                                 uint64_t audio_clock_us) {
	if (!st) return;
	slots_lock(st);
	st->observed_clock_us = audio_clock_us;
	/* The caller enters this path only after the preceding frame's
	 * vita2d_wait_rendering_done(), so displaying/retiring slots no longer
	 * have a live GPU reader and can be returned immediately. */
	int released_display = st->display_slot;
	if (released_display >= 0) {
		SoftwareVideoFrameSlot *slot = &st->slots[released_display];
		if (st->zero_copy && slot->frame) av_frame_unref(slot->frame);
		slot->state = SLOT_FREE;
		st->display_slot = -1;
	}
	if (st->retire_slot >= 0 && st->retire_slot != released_display) {
		SoftwareVideoFrameSlot *slot = &st->slots[st->retire_slot];
		if (st->zero_copy && slot->frame) av_frame_unref(slot->frame);
		slot->state = SLOT_FREE;
	}
	st->retire_slot = -1;
	for (int i = 0; i < st->queue_capacity; i++) {
		SoftwareVideoFrameSlot *slot = &st->slots[i];
		if (slot->state == SLOT_READY && slot->pts_us <= audio_clock_us) {
			if (st->zero_copy && slot->frame) av_frame_unref(slot->frame);
			slot->state = SLOT_FREE;
			st->frames_dropped++;
		}
	}
	slots_unlock(st);
}

void vita_sw_software_video_buffer_status(SoftwareVideoState *st, int *ready_frames,
	                              int *have_display,
	                              uint64_t *display_pts_us) {
	if (ready_frames) *ready_frames = 0;
	if (have_display) *have_display = 0;
	if (display_pts_us) *display_pts_us = 0;
	if (!st) return;
	slots_lock(st);
	int ready = 0;
	uint64_t earliest_ready_pts_us = UINT64_MAX;
	for (int i = 0; i < st->queue_capacity; i++) {
		if (st->slots[i].state != SLOT_READY) continue;
		ready++;
		if (st->slots[i].pts_us < earliest_ready_pts_us)
			earliest_ready_pts_us = st->slots[i].pts_us;
	}
	if (ready_frames) *ready_frames = ready;
	if (st->display_slot >= 0) {
		if (have_display) *have_display = 1;
		if (display_pts_us)
			*display_pts_us = st->slots[st->display_slot].pts_us;
	} else if (display_pts_us && earliest_ready_pts_us != UINT64_MAX)
		*display_pts_us = earliest_ready_pts_us;
	slots_unlock(st);
}

void vita_sw_software_video_debug_snapshot(SoftwareVideoState *st,
	                               SoftwareVideoDebugSnapshot *snapshot) {
	if (!snapshot) return;
	memset(snapshot, 0, sizeof(*snapshot));
	if (!st) return;
	slots_lock(st);
	for (int i = 0; i < st->queue_capacity; i++)
		if (st->slots[i].state == SLOT_READY) snapshot->ready_frames++;
	snapshot->have_display = st->display_slot >= 0;
	snapshot->source_fps = st->source_fps;
	snapshot->hardware_accelerated = st->hardware_accelerated;
	snapshot->direct_rendering = st->hardware_accelerated && st->zero_copy;
	snapshot->queue_capacity = st->queue_capacity;
	snapshot->catchup_nonref = st->catchup_nonref || st->catchup_drop_active;
	snapshot->ring_memory_bytes = st->ring_memory_bytes;
	snapshot->packets_read = st->packets_read;
	snapshot->frames_decoded = st->frames_decoded;
	snapshot->frames_published = st->next_serial;
	snapshot->frames_shown = st->frames_shown;
	snapshot->frames_dropped = st->frames_dropped;
	snapshot->frames_late_dropped = st->frames_late_dropped;
	snapshot->hardware_pts_repairs = st->hardware_pts_repairs;
	snapshot->presentation_cadence_breaks = st->presentation_cadence_breaks;
	snapshot->presentation_gap_max_us = st->presentation_gap_max_us;
	st->presentation_gap_max_us = 0;
	snapshot->decoded_pts_us = st->last_decoded_pts_us;
	snapshot->decode_time_us = st->decode_time_us;
	snapshot->copy_time_us = st->copy_time_us;
	slots_unlock(st);
}

void vita_sw_software_video_render_complete(SoftwareVideoState *st) {
	if (!st) return;
	slots_lock(st);
	if (st->retire_slot >= 0) {
		if (st->zero_copy && st->slots[st->retire_slot].frame)
			av_frame_unref(st->slots[st->retire_slot].frame);
		st->slots[st->retire_slot].state = SLOT_FREE;
		st->retire_slot = -1;
	}
	slots_unlock(st);
}

void vita_sw_software_video_join(SoftwareVideoState *st) {
	if (!st) return;
	st->stop = 1;
	if (st->started) {
		int join_ret = pthread_join(st->thread, NULL);
		if (join_ret != 0) {
			log_printf("software video: pthread_join -> %d", join_ret);
			while (!st->thread_done)
				sceKernelDelayThread(SOFTWARE_VIDEO_POLL_US);
			__sync_synchronize();
		}
	}
	/* A slot may still be referenced by the most recently queued draw. */
	vita2d_wait_rendering_done();
	log_printf("software video: join frames=%u shown=%u dropped=%u eof=%d err=%d",
	           st->frames_decoded, st->frames_shown, st->frames_dropped,
	           st->eof, st->had_error);
	release_frame_buffers(st);
	st->started = 0;
}

void vita_sw_software_video_join_for_restart(SoftwareVideoState *st) {
	/* Keep the working full teardown semantics across seek/reopen operations. */
	vita_sw_software_video_join(st);
}

void vita_sw_software_video_join_after_gpu_fence(SoftwareVideoState *st) {
	vita_sw_software_video_join(st);
}
