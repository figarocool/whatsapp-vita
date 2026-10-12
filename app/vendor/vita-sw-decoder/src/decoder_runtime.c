#include "decoder_runtime.h"

/* The software package intentionally has no ReAvPlayer/h264_vita runtime
 * dependency. Keeping the function preserves the common public API. */
int vita_sw_decoder_runtime_prepare(void) {
	return 0;
}
