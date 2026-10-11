#pragma once
#include <stdint.h>
#include <stddef.h>
// Decodifica un vocale OGG/Opus (quelli di WhatsApp) in WAV PCM 16 bit mono a 16 kHz. Ritorna 0 e un buffer malloc; -1 se non e' OGG/Opus.
int audio_ogg_opus_to_wav(const uint8_t *in, size_t n, uint8_t **out, size_t *outn, double *seconds);
// Controllo grossolano di un MP4: 1 = H.264 (o niente video), 0 = HEVC (la Vita non lo decodifica)
int video_mp4_playable(const uint8_t *in, size_t n);
// Informazioni di un MP4: codec video ('avc1', 'hvc1'...), risoluzione, profilo/livello H.264. Ritorna 0 se ha trovato un video.
typedef struct { char codec[8]; int width, height, profile, level; } Mp4Info;
int video_mp4_info(const uint8_t *in, size_t n, Mp4Info *out);
