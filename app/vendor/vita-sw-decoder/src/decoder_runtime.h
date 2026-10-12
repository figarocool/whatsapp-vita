#ifndef VITA_SW_DECODER_DECODER_RUNTIME_H
#define VITA_SW_DECODER_DECODER_RUNTIME_H

/* Loads the bundled compatibility runtime once for the process. The runtime
 * must remain resident until every player and SceAvPlayer instance is closed. */
int vita_sw_decoder_runtime_prepare(void);

#endif
