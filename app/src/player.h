// Lettore video/audio interno (SceAvPlayer del sistema): riproduce MP4 H.264/AAC gia' convertiti per la Vita.
#pragma once
#include <vita2d.h>
int player_open(const char *path, int loop);   // 0 ok
int player_open_software(const char *path, int loop);
int player_draw_software(float x, float y, float w, float h); // -1: system player
int player_software_failed(void);
void player_render_complete(void);
unsigned long long player_time_ms(void);   // posizione di riproduzione
int player_ended(void);   // 1 se la riproduzione (senza loop) e' finita
void player_close(void);
int player_active(void);
// da chiamare a ogni frame: aggiorna l'immagine; ritorna la texture corrente (o NULL se non c'e' ancora un fotogramma)
vita2d_texture *player_frame(void);
unsigned player_frames(void);   // fotogrammi ricevuti finora (diagnostica)
void player_size(unsigned *w, unsigned *h);   // dimensioni dell'ultimo fotogramma
void player_audio_diag(char *out, int n);   // stato dell'audio (diagnostica)
void player_video_diag(char *out, int n);
void player_stream_diag(char *out, int n);
