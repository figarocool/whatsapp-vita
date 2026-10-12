/*
 * Vita HW Player - direct hardware AAC decoding (sceAudiodec).
 * See vita_aac_decoder.h for the ownership and buffer contract.
 */

#include "internal/vita_aac_decoder.h"

#include <string.h>

#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>

#include "module_log.h"

static void *allocate_uncached(uint32_t size, SceUID *out_memblock) {
	/* SCE_AUDIODEC_ROUND_UP aligns to 0x100, which is enough for the decoder
	 * but NOT for sceKernelAllocMemBlock, which rejects sizes that are not a
	 * multiple of 4 KiB: the 1536-byte AAC ES buffer failed to allocate and
	 * the init exited with a silent -1 (fifth hardware test 2026-08-07,
	 * ret=0xFFFFFFFF with no intermediate log at all — this path was the only
	 * possible -1 return without a log). The memblock alignment is
	 * independent of the maxEsSize/maxPcmSize passed to the decoder, which
	 * stay the logical ones. */
	size = SCE_AUDIODEC_ROUND_UP(size);
	uint32_t memblock_size = (size + 0xFFFu) & ~0xFFFu;
	SceUID memblock = sceKernelAllocMemBlock("VitaSwDecoderAacBuf",
	                                         SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
	                                         memblock_size, NULL);
	if (memblock < 0) {
		log_printf("vita_aac: sceKernelAllocMemBlock(%u bytes) -> 0x%08X",
		          memblock_size, (unsigned)memblock);
		return NULL;
	}
	void *base = NULL;
	if (sceKernelGetMemBlockBase(memblock, &base) < 0 || !base) {
		sceKernelFreeMemBlock(memblock);
		return NULL;
	}
	memset(base, 0, size);
	*out_memblock = memblock;
	return base;
}

static void teardown_partial(VitaAacDecoder *dec) {
	if (dec->decoder_created) {
		sceAudiodecDeleteDecoder(&dec->ctrl);
		dec->decoder_created = 0;
	}
	if (dec->es_memblock >= 0) {
		sceKernelFreeMemBlock(dec->es_memblock);
		dec->es_memblock = -1;
		dec->es_buf = NULL;
	}
	if (dec->pcm_memblock >= 0) {
		sceKernelFreeMemBlock(dec->pcm_memblock);
		dec->pcm_memblock = -1;
		dec->pcm_buf = NULL;
	}
	if (dec->library_initialized) {
		sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC);
		dec->library_initialized = 0;
	}
	if (dec->module_loaded) {
		sceSysmoduleUnloadModule(SCE_SYSMODULE_AVCDEC);
		dec->module_loaded = 0;
	}
}

int vita_sw_aac_decoder_init_ex(VitaAacDecoder *dec, uint32_t channels,
	                         uint32_t sample_rate, int is_adts) {
	if (!dec || !channels || channels > VITA_AAC_MAX_CHANNELS || !sample_rate) return -1;
	memset(dec, 0, sizeof(*dec));
	dec->es_memblock = -1;
	dec->pcm_memblock = -1;
	dec->channels = channels;
	dec->sample_rate = sample_rate;

	/* "Best effort" attempt, NEVER fatal (2026-08-06): both
	 * SCE_SYSMODULE_AUDIOCODEC (0x0037) and SCE_SYSMODULE_AVCDEC (0x0054)
	 * returned SCE_SYSMODULE_ERROR_INVALID_VALUE (0x805A1000) on real
	 * hardware — either neither of them was the real cause, or the module is
	 * already made available elsewhere (e.g. by reAvPlayer.suprx/sceAvPlayer,
	 * already initialized elsewhere in this same app session for the mini-
	 * player). wiliwili (xfangfang/wiliwili, vitadec_audio.c) does not call
	 * sceSysmoduleLoadModule here at all, but that is not sufficient
	 * guarantee in isolation (their bootstrap may load AVCDEC elsewhere).
	 * Therefore: we try, we log the outcome whatever it is, but we do not
	 * block initialization on its failure — sceAudiodecInitLibrary right
	 * below is the real judge of whether the module is needed and whether it
	 * is present: if that fails in turn, the error will be more specific
	 * (SCE_AUDIODEC_ERROR_NOT_INITIALIZED or similar) than a generic
	 * INVALID_VALUE, and diagnostic for the next step. */
	int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
	log_printf("vita_aac: sceSysmoduleLoadModule(AVCDEC) -> 0x%08X (non-fatal)",
	          (unsigned)ret);
	if (ret >= 0) dec->module_loaded = 1;

	SceAudiodecInitParam init_param;
	memset(&init_param, 0, sizeof(init_param));
	init_param.aac.size = sizeof(init_param.aac);
	init_param.aac.totalStreams = 1;
	ret = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &init_param);
	if (ret < 0) {
		log_printf("vita_aac: sceAudiodecInitLibrary -> 0x%08X", (unsigned)ret);
		teardown_partial(dec);
		return ret;
	}
	dec->library_initialized = 1;

	uint32_t es_size = SCE_AUDIODEC_ROUND_UP(SCE_AUDIODEC_AAC_MAX_ES_SIZE);
	dec->es_buf = allocate_uncached(es_size, &dec->es_memblock);
	if (!dec->es_buf) {
		log_printf("vita_aac: ES buffer allocation failed (%u bytes)", es_size);
		teardown_partial(dec);
		return -1;
	}

	dec->pcm_buf_size = SCE_AUDIODEC_ROUND_UP(
	    channels * SCE_AUDIODEC_AAC_MAX_SAMPLES * (uint32_t)sizeof(int16_t));
	dec->pcm_buf = (int16_t *)allocate_uncached(dec->pcm_buf_size, &dec->pcm_memblock);
	if (!dec->pcm_buf) {
		log_printf("vita_aac: PCM buffer allocation failed (%u bytes)", dec->pcm_buf_size);
		teardown_partial(dec);
		return -1;
	}

	memset(&dec->info, 0, sizeof(dec->info));
	dec->info.aac.size = sizeof(dec->info.aac);
	dec->info.aac.isAdts = is_adts ? 1 : 0;
	dec->info.aac.ch = channels;
	dec->info.aac.samplingRate = sample_rate;
	dec->info.aac.isSbr = 1; /* not inferable from the ES header, see the comment in vita_aac_decoder.h */

	memset(&dec->ctrl, 0, sizeof(dec->ctrl));
	dec->ctrl.size = sizeof(dec->ctrl);
	dec->ctrl.pEs = dec->es_buf;
	dec->ctrl.maxEsSize = es_size;
	dec->ctrl.pPcm = dec->pcm_buf;
	dec->ctrl.maxPcmSize = dec->pcm_buf_size;
	dec->ctrl.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
	dec->ctrl.pInfo = &dec->info;

	ret = sceAudiodecCreateDecoder(&dec->ctrl, SCE_AUDIODEC_TYPE_AAC);
	if (ret < 0) {
		log_printf("vita_aac: sceAudiodecCreateDecoder -> 0x%08X", (unsigned)ret);
		teardown_partial(dec);
		return ret;
	}
	dec->decoder_created = 1;

	log_printf("vita_aac: decoder ready ch=%u rate=%u ADTS=%d (esBuf=%u pcmBuf=%u)",
	          channels, sample_rate, is_adts ? 1 : 0, es_size,
	          dec->pcm_buf_size);
	return 0;
}

int vita_sw_aac_decoder_init(VitaAacDecoder *dec, uint32_t channels,
	                      uint32_t sample_rate) {
	return vita_sw_aac_decoder_init_ex(dec, channels, sample_rate, 0);
}

int vita_sw_aac_decoder_decode(VitaAacDecoder *dec, const uint8_t *es_data,
                            size_t es_size, VitaAacDecodedAudio *out) {
	if (!dec || !dec->decoder_created || !es_data || !es_size || !out) return -1;
	if (es_size > dec->ctrl.maxEsSize) {
		log_printf("vita_aac: %u-byte AAC frame exceeds %u-byte ES buffer",
		          (unsigned)es_size, dec->ctrl.maxEsSize);
		return -1;
	}
	memcpy(dec->es_buf, es_data, es_size);
	dec->ctrl.inputEsSize = (SceUInt32)es_size;
	dec->ctrl.outputPcmSize = 0;

	int ret = sceAudiodecDecode(&dec->ctrl);
	if (ret < 0) return ret;
	if (dec->ctrl.outputPcmSize == 0) return 0;

	out->pcm = dec->pcm_buf;
	out->channels = dec->channels;
	out->sample_rate = dec->sample_rate;
	out->frames = dec->ctrl.outputPcmSize / (uint32_t)(dec->channels * sizeof(int16_t));
	return 1;
}

void vita_sw_aac_decoder_term(VitaAacDecoder *dec) {
	if (!dec) return;
	teardown_partial(dec);
	memset(dec, 0, sizeof(*dec));
	dec->es_memblock = -1;
	dec->pcm_memblock = -1;
}
