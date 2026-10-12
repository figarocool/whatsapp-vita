#include "internal/elementary_audio.h"

#include <malloc.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

#include "module_log.h"

/* Keep the blocking AudioOut consumer above video and the demux/decode producer
 * below it. The bounded PCM ring absorbs occasional transport and AAC decode
 * stalls without making the hardware port wait for the next grain. */
#define ELEMENTARY_AUDIO_OUTPUT_PRIORITY 64
#define ELEMENTARY_AUDIO_THREAD_PRIORITY 66
#define ELEMENTARY_AUDIO_THREAD_STACK    0x40000
#define ELEMENTARY_AUDIO_OUTPUT_STACK    0x10000
/* Vita AudioOut grain used by the hardware AAC output loop. */
#define ELEMENTARY_AUDIO_GRAIN ELEMENTARY_AUDIO_GRAIN_FRAMES
/* Short wait while paused: it is not pacing, just to avoid spinning the CPU
 * while the thread stays alive without producing output. */
#define ELEMENTARY_AUDIO_PAUSE_DELAY_US (10 * 1000)

static int first_audio_stream(const AVFormatContext *ctx) {
	for (unsigned int i = 0; ctx && i < ctx->nb_streams; i++) {
		if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
			return (int)i;
	}
	return -1;
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

static int parse_adts_header(const uint8_t *data, size_t size,
	                         uint32_t *channels, uint32_t *sample_rate) {
	static const uint32_t rates[13] = {
		96000, 88200, 64000, 48000, 44100, 32000, 24000,
		22050, 16000, 12000, 11025, 8000, 7350
	};
	if (!data || size < 7 || data[0] != 0xff || (data[1] & 0xf6) != 0xf0)
		return -1;
	unsigned rate_index = (data[2] >> 2) & 0x0f;
	unsigned channel_config = ((unsigned)(data[2] & 1) << 2) |
	                          ((unsigned)data[3] >> 6);
	if (rate_index >= 13 || channel_config < 1 || channel_config > 2)
		return -1;
	if (channels) *channels = channel_config;
	if (sample_rate) *sample_rate = rates[rate_index];
	return 0;
}

/* Reads the next packet of the selected audio track, silently discarding
 * packets belonging to other tracks that may be present in the same
 * AVFormatContext. Returns
 * 1 if *packet was filled in, 0 at EOF, <0 on error (including the abort due
 * to cancel: the interrupt_callback is already attached to the
 * AVFormatContext by the caller, see elementary_audio.h). */
static int read_audio_packet(ElementaryAudioState *st, AVPacket *packet) {
	if (st->prefetched_packet) {
		if (st->stop) return AVERROR_EXIT;
		av_packet_move_ref(packet, st->prefetched_packet);
		av_packet_free(&st->prefetched_packet);
		return 1;
	}
	int ret = AVERROR_EXIT;
	while (!st->stop && (ret = av_read_frame(st->demux, packet)) >= 0) {
		if (packet->stream_index == st->stream_index) return 1;
		av_packet_unref(packet);
	}
	return st->stop ? AVERROR_EXIT : ret == AVERROR_EOF ? 0 : ret;
}

static int vita_sw_elementary_audio_output_thread(SceSize args, void *argp) {
	(void)args;
	ElementaryAudioState *st = *(ElementaryAudioState **)argp;
	uint64_t last_output_wall_us = 0;
	uint64_t maximum_output_gap_us = 0;
	unsigned int output_grains = 0;
	unsigned int delayed_output_grains = 0;
	int last_applied_volume = -1;
	log_printf("elementary_audio: output priority=%d ring=%d",
	           sceKernelGetThreadCurrentPriority(), ELEMENTARY_AUDIO_RING_GRAINS);

	while (st->start_gate && !*st->start_gate && !*st->cancel && !st->stop)
		sceKernelDelayThread(1000);
	while (!*st->cancel && !st->stop) {
		if (st->paused) {
			last_output_wall_us = 0;
			sceKernelDelayThread(ELEMENTARY_AUDIO_PAUSE_DELAY_US);
			continue;
		}
		uint32_t read_seq = st->pcm_read_seq;
		if (read_seq == st->pcm_write_seq) {
			if (st->producer_done) break;
			sceKernelDelayThread(1000);
			continue;
		}
		int requested_volume = st->volume_percent;
		if (requested_volume != last_applied_volume && st->port >= 0) {
			last_applied_volume = requested_volume;
			int hardware_percent = requested_volume > 100 ? 100 : requested_volume;
			int hardware_volume = hardware_percent * SCE_AUDIO_VOLUME_0DB / 100;
			int volume[2] = { hardware_volume, hardware_volume };
			sceAudioOutSetVolume(st->port,
			                      SCE_AUDIO_VOLUME_FLAG_L_CH |
			                          SCE_AUDIO_VOLUME_FLAG_R_CH,
			                      volume);
		}
		uint32_t slot = read_seq % ELEMENTARY_AUDIO_RING_GRAINS;
		int16_t *grain = st->pcm_ring +
		                 slot * ELEMENTARY_AUDIO_GRAIN *
		                     ELEMENTARY_AUDIO_MAX_CHANNELS;
		int output_ret = sceAudioOutOutput(st->port, grain);
		if (output_ret < 0) {
			log_printf("elementary_audio: sceAudioOutOutput -> 0x%08X",
			           (unsigned)output_ret);
			st->had_error = 1;
			st->stop = 1;
			break;
		}
		uint64_t output_wall_us = sceKernelGetProcessTimeWide();
		if (last_output_wall_us) {
			uint64_t gap_us = output_wall_us - last_output_wall_us;
			uint64_t expected_us =
			    (uint64_t)ELEMENTARY_AUDIO_GRAIN * 1000000ULL / st->sample_rate;
			if (gap_us > maximum_output_gap_us) maximum_output_gap_us = gap_us;
			if (gap_us > expected_us + 4000ULL) delayed_output_grains++;
		}
		last_output_wall_us = output_wall_us;
		output_grains++;
		if (st->pcm_pts_valid[slot]) st->played_until_ms = st->pcm_end_ms[slot];
		__sync_synchronize();
		st->pcm_read_seq = read_seq + 1;
	}
	if (output_grains)
		log_printf("elementary_audio: cadence grains=%u delayed=%u gap_max=%llu us underrun_waits=buffered",
		           output_grains, delayed_output_grains,
		           (unsigned long long)maximum_output_gap_us);
	return 0;
}

static int vita_sw_elementary_audio_thread(SceSize args, void *argp) {
	(void)args;
	ElementaryAudioState *st = *(ElementaryAudioState **)argp;

	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		st->had_error = 1;
		__sync_synchronize();
		st->thread_done = 1;
		return 0;
	}

	const AVRational stream_tb = st->demux->streams[st->stream_index]->time_base;
	const AVRational us_tb = { 1, 1000000 };
	int decode_attempts = 0;
	int have_packet = 0;
	int reached_eof = 0;
	log_printf("elementary_audio: producer priority=%d",
	           sceKernelGetThreadCurrentPriority());
	/* Prime one selected AU while output is still gated. Besides overlapping the
	 * demux read with video startup, this gives a track-switch transaction an exact
	 * readiness point: clean EOF here means the requested track has no audio at the
	 * current media position and must not be committed. */
	while (!*st->cancel && !st->stop && !have_packet) {
		int ret = read_audio_packet(st, packet);
		if (ret == 0) {
			__sync_synchronize();
			st->eof = 1;
			goto done;
		}
		if (ret < 0) {
			if (!*st->cancel && !st->stop) {
				log_printf("elementary_audio: initial av_read_frame -> %d", ret);
				st->had_error = 1;
			}
			goto done;
		}
		int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
		int64_t pts_us = pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE
		               : av_rescale_q(pts, stream_tb, us_tb) -
		                     st->timeline_origin_us;
		int64_t pts_ms = pts_us == AV_NOPTS_VALUE ? -1 : pts_us / 1000;
		uint64_t requested_ms = st->initial_position_ms;
		if (requested_ms > 0 &&
		    (pts_ms < 0 || (uint64_t)pts_ms < requested_ms)) {
			av_packet_unref(packet);
			continue;
		}
		if (pts_ms >= 0) {
			st->first_packet_pts_ms = (uint64_t)pts_ms > UINT32_MAX
			                         ? UINT32_MAX : (uint32_t)pts_ms;
			st->first_packet_pts_valid = 1;
		}
		log_printf("elementary_audio: landing AU=%lld ms requested=%llu ms",
		           (long long)pts_ms, (unsigned long long)requested_ms);
		st->initial_position_ms = 0;
		have_packet = 1;
	}
	if (*st->cancel || st->stop) goto done;
	st->output_thid = sceKernelCreateThread(
	    "VitaSwDecoderAudioOut", vita_sw_elementary_audio_output_thread,
	    ELEMENTARY_AUDIO_OUTPUT_PRIORITY, ELEMENTARY_AUDIO_OUTPUT_STACK, 0, 0, NULL);
	if (st->output_thid < 0) {
		st->had_error = 1;
		goto done;
	}
	void *output_self = st;
	int output_start_ret = sceKernelStartThread(
	    st->output_thid, sizeof(output_self), &output_self);
	if (output_start_ret < 0) {
		log_printf("elementary_audio: output start -> 0x%08X",
		           (unsigned)output_start_ret);
		sceKernelDeleteThread(st->output_thid);
		st->output_thid = -1;
		st->had_error = 1;
		goto done;
	}
	st->output_started = 1;
	__sync_synchronize();
	st->first_packet_ready = 1;

	while (!*st->cancel && !st->stop) {
		int requested_volume = st->volume_percent;
		if (st->paused) {
			/* Let the consumer own pause semantics; stop producing once the ring
			 * is full so decoded PCM remains bounded and ordered. */
			sceKernelDelayThread(ELEMENTARY_AUDIO_PAUSE_DELAY_US);
		}

		if (have_packet) {
			have_packet = 0;
		} else {
			int ret = read_audio_packet(st, packet);
			if (ret == 0) {
				/* EOF belongs to the public playback clock only after AudioOut has
				 * consumed every queued grain. Otherwise video and subtitles would
				 * hand off to wall time while buffered audio is still playing. */
				reached_eof = 1;
				break;
			}
			if (ret < 0) {
				/* If cancel is already raised, the error is almost certainly the
				 * interrupt_callback abort: it is not a decode failure, so we
				 * exit cleanly without had_error. */
				if (!*st->cancel && !st->stop) {
					log_printf("elementary_audio: av_read_frame -> %d", ret);
					st->had_error = 1;
				}
				break;
			}
		}

		VitaAacDecodedAudio decoded;
		memset(&decoded, 0, sizeof(decoded));
		int decode_ret = vita_sw_aac_decoder_decode(&st->decoder, packet->data,
		                                         (size_t)packet->size, &decoded);
		if (decode_ret < 0) {
			log_printf("elementary_audio: vita_sw_aac_decoder_decode -> %d (attempt #%d)",
			          decode_ret, decode_attempts);
			av_packet_unref(packet);
			/* Error not on the first packet: fatal, SS5 point 7. On the first
			 * packet it is tolerated (hardware decoder warm-up) and we carry
			 * on. */
			if (decode_attempts > 0) {
				st->had_error = 1;
				break;
			}
			decode_attempts++;
			continue;
		}
		decode_attempts++;

		/* The pts must be read before av_packet_unref(): unref clears it. */
		int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
		av_packet_unref(packet);

		if (decode_ret == 0 || !decoded.pcm || decoded.frames == 0) {
			/* No samples produced for this packet: it is not an error
			 * (symmetry with the video decoder, SS5 point 2), we keep
			 * feeding packets. */
			continue;
		}
		/* A pause that lands after decode must not discard this access unit.
		 * The per-grain loop below holds the decoded PCM until resume. */
		if (*st->cancel || st->stop) continue;

		/* The AudioOut port consumes exactly the grain chosen at open (1024
		 * samples/channel). SBR can make one AAC access unit produce 2048
		 * samples, which must be delivered as two consecutive grains; passing
		 * the base pointer only once would discard half the PCM while moving the
		 * clock by the full packet. AAC-LC/SBR are expected to be exact
		 * multiples here; fail closed on any other shape instead of corrupting
		 * sync silently. */
		if (decoded.frames % ELEMENTARY_AUDIO_GRAIN != 0) {
			log_printf("elementary_audio: PCM not aligned to output grain (%u frames)",
			          decoded.frames);
			st->had_error = 1;
			break;
		}
		/* Same 100..200% software boost as the direct player. This PCM buffer
		 * belongs to our decoder and isn't reused until the next decode, which
		 * happens only after all blocking AudioOut grains below have returned. */
		if (requested_volume > 100) {
			int16_t *pcm = (int16_t *)(uintptr_t)decoded.pcm;
			uint32_t samples = decoded.frames * decoded.channels;
			for (uint32_t i = 0; i < samples; i++) {
				int32_t value = (int32_t)pcm[i] * requested_volume / 100;
				if (value > INT16_MAX) value = INT16_MAX;
				else if (value < INT16_MIN) value = INT16_MIN;
				pcm[i] = (int16_t)value;
			}
		}
		int64_t pts_us = pts != AV_NOPTS_VALUE
		               ? av_rescale_q(pts, stream_tb, us_tb) -
		                     st->timeline_origin_us
		               : AV_NOPTS_VALUE;
		for (uint32_t offset = 0; offset < decoded.frames;
		     offset += ELEMENTARY_AUDIO_GRAIN) {
			while (st->pcm_write_seq - st->pcm_read_seq >=
			           ELEMENTARY_AUDIO_RING_GRAINS &&
			       !*st->cancel && !st->stop)
				sceKernelDelayThread(1000);
			if (*st->cancel || st->stop) break;
			uint32_t write_seq = st->pcm_write_seq;
			uint32_t slot = write_seq % ELEMENTARY_AUDIO_RING_GRAINS;
			int16_t *ring_grain = st->pcm_ring +
			                      slot * ELEMENTARY_AUDIO_GRAIN *
			                          ELEMENTARY_AUDIO_MAX_CHANNELS;
			memcpy(ring_grain,
			       decoded.pcm + offset * st->decoder.channels,
			       ELEMENTARY_AUDIO_GRAIN * st->decoder.channels * sizeof(int16_t));
			st->pcm_pts_valid[slot] = pts_us >= 0;
			if (pts_us >= 0) {
				uint64_t end_us = (uint64_t)pts_us +
				    (uint64_t)(offset + ELEMENTARY_AUDIO_GRAIN) * 1000000ULL /
				    st->sample_rate;
				st->pcm_end_ms[slot] = end_us / 1000ULL > UINT32_MAX
				                     ? UINT32_MAX : (uint32_t)(end_us / 1000ULL);
			}
			__sync_synchronize();
			st->pcm_write_seq = write_seq + 1;
		}
		if (st->had_error || *st->cancel || st->stop) break;
	}

done:
	__sync_synchronize();
	st->producer_done = 1;
	if (st->output_started && st->output_thid >= 0) {
		sceKernelWaitThreadEnd(st->output_thid, NULL, NULL);
		sceKernelDeleteThread(st->output_thid);
		st->output_thid = -1;
		st->output_started = 0;
	}
	if (reached_eof && !st->had_error && !*st->cancel && !st->stop) {
		__sync_synchronize();
		st->eof = 1;
	}
	av_packet_free(&packet);
	__sync_synchronize();
	st->thread_done = 1;
	return 0;
}

int vita_sw_elementary_audio_start(ElementaryAudioState *st, AVFormatContext *demux,
                           int stream_index, uint64_t initial_position_ms,
                           volatile int *cancel,
                           volatile int *start_gate) {
	if (!st || !demux || !cancel) return -1;
	memset(st, 0, sizeof(*st));
	st->port = -1;
	st->thid = -1;
	st->output_thid = -1;
	st->demux = demux;
	st->cancel = cancel;
	st->start_gate = start_gate;
	st->initial_position_ms = initial_position_ms;
	st->timeline_origin_us = media_timeline_origin_us(demux);
	st->volume_percent = 100;

	if (stream_index < 0) stream_index = first_audio_stream(demux);
	if (stream_index < 0 || (unsigned int)stream_index >= demux->nb_streams ||
	    demux->streams[stream_index]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
		log_printf("elementary_audio: no valid audio track (stream_index=%d)",
		          stream_index);
		return -1;
	}
	st->stream_index = stream_index;

	const AVCodecParameters *params = demux->streams[stream_index]->codecpar;
	uint32_t channels = params->ch_layout.nb_channels > 0
	                   ? (uint32_t)params->ch_layout.nb_channels : 0;
	uint32_t sample_rate = params->sample_rate > 0 ? (uint32_t)params->sample_rate : 0;
	/* MPEG-TS carries AAC as ADTS. FFmpeg can identify the stream while still
	 * leaving codecpar channels/rate empty after a short probe.
	 * Inspect one AU directly, retain it for the worker, and configure
	 * sceAudiodec for ADTS instead of rejecting an otherwise valid live. */
	if (params->codec_id == AV_CODEC_ID_AAC &&
	    (!channels || !sample_rate || params->extradata_size <= 0)) {
		AVPacket *probe = av_packet_alloc();
		int probe_ret = probe ? read_audio_packet(st, probe) : AVERROR(ENOMEM);
		if (probe_ret > 0) {
			uint32_t adts_channels = 0, adts_rate = 0;
			if (parse_adts_header(probe->data, (size_t)probe->size,
			                      &adts_channels, &adts_rate) == 0) {
				st->input_is_adts = 1;
				if (!channels) channels = adts_channels;
				if (!sample_rate) sample_rate = adts_rate;
				log_printf("elementary_audio: ADTS detected ch=%u rate=%u packet=%d",
				          adts_channels, adts_rate, probe->size);
			}
			st->prefetched_packet = probe;
		} else {
			av_packet_free(&probe);
			if ((!channels || !sample_rate) && probe_ret < 0)
				log_printf("elementary_audio: probe ADTS -> %d", probe_ret);
		}
	}
	if (!channels || !sample_rate) {
		log_printf("elementary_audio: missing codec parameters (ch=%u rate=%u)",
		          channels, sample_rate);
		av_packet_free(&st->prefetched_packet);
		return -1;
	}
	st->sample_rate = sample_rate;
	size_t ring_bytes = ELEMENTARY_AUDIO_RING_GRAINS *
	                    ELEMENTARY_AUDIO_GRAIN *
	                    ELEMENTARY_AUDIO_MAX_CHANNELS * sizeof(int16_t);
	st->pcm_ring = memalign(64, ring_bytes);
	if (!st->pcm_ring) {
		av_packet_free(&st->prefetched_packet);
		return AVERROR(ENOMEM);
	}
	memset(st->pcm_ring, 0, ring_bytes);

	/* SBR state is selected inside vita_sw_aac_decoder_init_ex. A future decoder
	 * revision can parse the complete AudioSpecificConfig when required. */
	int ret = vita_sw_aac_decoder_init_ex(&st->decoder, channels, sample_rate,
	                                  st->input_is_adts);
	if (ret < 0) {
		log_printf("elementary_audio: vita_sw_aac_decoder_init -> 0x%08X", (unsigned)ret);
		av_packet_free(&st->prefetched_packet);
		free(st->pcm_ring);
		st->pcm_ring = NULL;
		return ret;
	}
	st->decoder_ready = 1;

	int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, ELEMENTARY_AUDIO_GRAIN,
	                               (int)sample_rate,
	                               channels == 1 ? SCE_AUDIO_OUT_MODE_MONO
	                                             : SCE_AUDIO_OUT_MODE_STEREO);
	log_printf("elementary_audio: sceAudioOutOpenPort -> 0x%08X (ch=%u rate=%u)",
	          (unsigned)port, channels, sample_rate);
	if (port < 0) {
		vita_sw_aac_decoder_term(&st->decoder);
		st->decoder_ready = 0;
		av_packet_free(&st->prefetched_packet);
		free(st->pcm_ring);
		st->pcm_ring = NULL;
		return port;
	}
	st->port = port;
	st->port_open = 1;
	int volume[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
	sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
	                     volume);

	st->thid = sceKernelCreateThread("VitaSwDecoderPlayerAudio", vita_sw_elementary_audio_thread,
	                                 ELEMENTARY_AUDIO_THREAD_PRIORITY,
	                                 ELEMENTARY_AUDIO_THREAD_STACK, 0, 0, NULL);
	if (st->thid < 0) {
		ret = st->thid;
		sceAudioOutReleasePort(st->port);
		st->port = -1;
		st->port_open = 0;
		vita_sw_aac_decoder_term(&st->decoder);
		st->decoder_ready = 0;
		av_packet_free(&st->prefetched_packet);
		free(st->pcm_ring);
		st->pcm_ring = NULL;
		return ret;
	}

	void *self = st;
	ret = sceKernelStartThread(st->thid, sizeof(self), &self);
	if (ret < 0) {
		log_printf("elementary_audio: sceKernelStartThread -> 0x%08X", (unsigned)ret);
		sceKernelDeleteThread(st->thid);
		st->thid = -1;
		sceAudioOutReleasePort(st->port);
		st->port = -1;
		st->port_open = 0;
		vita_sw_aac_decoder_term(&st->decoder);
		st->decoder_ready = 0;
		av_packet_free(&st->prefetched_packet);
		free(st->pcm_ring);
		st->pcm_ring = NULL;
		return ret;
	}

	st->started = 1;
	return 0;
}

void vita_sw_elementary_audio_join(ElementaryAudioState *st) {
	if (!st) return;
	/* Each step is gated on its own state flag, not on the numeric value of
	 * the corresponding field: if vita_sw_elementary_audio_start() was never called
	 * (or failed) on a state zeroed by the caller, port/thid set to 0 would
	 * be indistinguishable from valid handles. */
	if (st->started && st->thid >= 0) sceKernelWaitThreadEnd(st->thid, NULL, NULL);
	if (st->port_open) {
		sceAudioOutReleasePort(st->port);
		st->port_open = 0;
	}
	if (st->decoder_ready) {
		/* vita_sw_aac_decoder_term() is not safe on a VitaAacDecoder that was
		 * never initialized: teardown_partial() reads es_memblock/pcm_memblock
		 * from it as already written by vita_sw_aac_decoder_init(), which on a
		 * zeroed struct would be 0 (an apparently valid memblock) instead of
		 * the expected -1 sentinel. decoder_ready is the only correct guard
		 * here. */
		vita_sw_aac_decoder_term(&st->decoder);
		st->decoder_ready = 0;
	}
	if (st->started && st->thid >= 0) sceKernelDeleteThread(st->thid);
	av_packet_free(&st->prefetched_packet);
	free(st->pcm_ring);
	st->pcm_ring = NULL;
	st->thid = -1;
	st->port = -1;
	st->started = 0;
}

uint64_t vita_sw_elementary_audio_clock_us(const ElementaryAudioState *st) {
	if (!st) return 0;
	return (uint64_t)st->played_until_ms * 1000ULL;
}

void vita_sw_elementary_audio_set_volume(ElementaryAudioState *st, int percent) {
	if (!st) return;
	if (percent < 0) percent = 0;
	if (percent > 300) percent = 300;
	st->volume_percent = percent;
}
