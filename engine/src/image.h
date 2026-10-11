#pragma once
#include <stdint.h>
#include <stddef.h>
// Decodifica JPEG/PNG/WebP, ridimensiona e ricodifica (JPEG o PNG). Sostituisce 'sharp' nel bridge.
typedef struct { int w, h; const char *fmt; } ImgInfo;
int img_info(const uint8_t *in, size_t n, ImgInfo *info);   // 0 ok
// fit: 0 = "inside" (resta dentro w x h, mantiene le proporzioni, senza ingrandire), 1 = "cover" (riempie esattamente w x h ritagliando al centro)
// out_fmt: 0 = JPEG, 1 = PNG. rotate: applica l'orientamento EXIF. Ritorna 0 e un buffer malloc in *out.
int img_convert(const uint8_t *in, size_t n, int max_w, int max_h, int fit, int out_fmt, int quality, int rotate, uint8_t **out, size_t *outn, int *ow, int *oh);
