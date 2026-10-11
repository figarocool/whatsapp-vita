// Lettore video/audio interno (SceAvPlayer del sistema): riproduce MP4 H.264/AAC gia' convertiti per la Vita.
#pragma once
#include <vita2d.h>
int player_open(const char *path);   // 0 ok
void player_close(void);
int player_active(void);
// da chiamare a ogni frame: aggiorna l'immagine; ritorna la texture corrente (o NULL se non c'e' ancora un fotogramma)
vita2d_texture *player_frame(void);
