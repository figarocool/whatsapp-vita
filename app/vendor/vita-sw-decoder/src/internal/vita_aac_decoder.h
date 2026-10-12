#ifndef VITA_SW_DECODER_AAC_DECODER_H
#define VITA_SW_DECODER_AAC_DECODER_H

#include <stdint.h>
#include <stddef.h>

#include <psp2/audiodec.h>
#include <psp2/kernel/sysmem.h>

/* Direct hardware AAC decoding (sceAudiodec), the audio counterpart of
 * The public constants come from the VitaSDK audio decoder headers.
 * Entirely public API, no *Internal variant. */

#define VITA_AAC_MAX_CHANNELS 2u /* SCE_AUDIODEC_AAC_MAX_CH_IN_DECODER */

typedef struct {
	SceAudiodecCtrl ctrl;
	SceAudiodecInfo info;
	SceUID es_memblock;
	SceUID pcm_memblock;
	uint8_t *es_buf;
	int16_t *pcm_buf;
	uint32_t pcm_buf_size;
	uint32_t channels;
	uint32_t sample_rate;
	int library_initialized;
	int decoder_created;
	int module_loaded;
} VitaAacDecoder;

typedef struct {
	const int16_t *pcm; /* interleaved, 16 bit, valid until the next decode() */
	uint32_t frames;    /* samples per channel */
	uint32_t channels;
	uint32_t sample_rate;
} VitaAacDecodedAudio;

/* Initializes the AAC decoder. channels/sample_rate are the ones declared by
 * the source container (the track's stsd/esds, already read elsewhere in the
 * project): sceAudiodec requires them as input, it does not infer them from
 * the stream. isSbr cannot be inferred from the ES header (an observation
 * reported by this session's research, not derivable a priori): it is always
 * assumed to be 1, as the only public reference found for this API on Vita
 * (wiliwili) does. Returns 0 on success. */
int vita_sw_aac_decoder_init(VitaAacDecoder *dec, uint32_t channels, uint32_t sample_rate);

/* Variant for transport streams, whose AAC access units retain their ADTS
 * headers. The original initializer remains the MP4/M4A-compatible wrapper. */
int vita_sw_aac_decoder_init_ex(VitaAacDecoder *dec, uint32_t channels,
	                         uint32_t sample_rate, int is_adts);

/* Decodes a single AAC access unit (raw, without an ADTS header: isAdts=0 at
 * init — the packets read by FFmpeg from an MP4/M4A track are already in this
 * form, one frame per packet). Returns 1 and fills in *out if PCM samples
 * were produced, 0 if the decoder produced no output for this frame (not
 * expected under normal conditions for AAC, handled anyway for symmetry with
 * the video decoder), <0 on error. *out points to a buffer owned by the
 * decoder, valid only until the next call. */
int vita_sw_aac_decoder_decode(VitaAacDecoder *dec, const uint8_t *es_data,
                            size_t es_size, VitaAacDecodedAudio *out);

void vita_sw_aac_decoder_term(VitaAacDecoder *dec);

#endif /* VITA_SW_DECODER_AAC_DECODER_H */
