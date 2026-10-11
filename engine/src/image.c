#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/demux.h>

typedef struct { uint8_t *px; int w, h; int alpha; } Bitmap;   // RGBA 8 bit
static void bm_free(Bitmap *b) { free(b->px); b->px = NULL; }

// ---------- JPEG ----------
typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } JErr;
static void jerr_exit(j_common_ptr c) { longjmp(((JErr *)c->err)->jb, 1); }
static int exif_orientation(const uint8_t *p, size_t n) {
  size_t i = 2; while (i + 4 < n && p[i] == 0xFF) { int m = p[i + 1]; size_t len = (p[i + 2] << 8) | p[i + 3]; if (m == 0xE1 && len > 14 && !memcmp(p + i + 4, "Exif\0\0", 6)) {
      const uint8_t *t = p + i + 10; size_t tl = len - 8; int le = t[0] == 'I'; if (tl < 8) return 1;
      #define R16(o) (le ? (t[o] | (t[o + 1] << 8)) : ((t[o] << 8) | t[o + 1]))
      #define R32(o) (le ? (t[o] | (t[o + 1] << 8) | (t[o + 2] << 16) | ((uint32_t)t[o + 3] << 24)) : (((uint32_t)t[o] << 24) | (t[o + 1] << 16) | (t[o + 2] << 8) | t[o + 3]))
      uint32_t off = R32(4); if (off + 2 > tl) return 1; int cnt = R16(off); for (int k = 0; k < cnt; k++) { size_t e = off + 2 + (size_t)k * 12; if (e + 12 > tl) break; if (R16(e) == 0x0112) { int v = R16(e + 8); return v >= 1 && v <= 8 ? v : 1; } }
      return 1; }
    if (m == 0xDA || m == 0xD9) break; i += 2 + len; }
  return 1;
}
static int jpeg_dims(const uint8_t *in, size_t n, int *w, int *h) {
  struct jpeg_decompress_struct c; JErr je; c.err = jpeg_std_error(&je.pub); je.pub.error_exit = jerr_exit; if (setjmp(je.jb)) { jpeg_destroy_decompress(&c); return -1; }
  jpeg_create_decompress(&c); jpeg_mem_src(&c, in, n); jpeg_read_header(&c, TRUE); *w = c.image_width; *h = c.image_height; jpeg_destroy_decompress(&c); return 0;
}
static int jpeg_decode(const uint8_t *in, size_t n, int want_w, int want_h, Bitmap *b) {
  struct jpeg_decompress_struct c; JErr je; c.err = jpeg_std_error(&je.pub); je.pub.error_exit = jerr_exit; uint8_t *row = NULL; b->px = NULL;
  if (setjmp(je.jb)) { jpeg_destroy_decompress(&c); free(row); free(b->px); b->px = NULL; return -1; }
  jpeg_create_decompress(&c); jpeg_mem_src(&c, in, n); jpeg_read_header(&c, TRUE); c.out_color_space = JCS_RGB;
  // decodifica gia' ridotta (1/2, 1/4, 1/8) quando basta: molto piu' veloce e leggera sulla Vita
  int denom = 1; while (denom < 8 && (int)c.image_width / (denom * 2) >= want_w && (int)c.image_height / (denom * 2) >= want_h) denom *= 2; c.scale_num = 1; c.scale_denom = denom;
  jpeg_start_decompress(&c); b->w = c.output_width; b->h = c.output_height; b->alpha = 0; b->px = malloc((size_t)b->w * b->h * 4); row = malloc((size_t)b->w * 3);
  if (!b->px || !row) longjmp(je.jb, 1);
  while (c.output_scanline < c.output_height) { uint8_t *rp = row; jpeg_read_scanlines(&c, &rp, 1); uint8_t *d = b->px + (size_t)(c.output_scanline - 1) * b->w * 4; for (int x = 0; x < b->w; x++) { d[x * 4] = row[x * 3]; d[x * 4 + 1] = row[x * 3 + 1]; d[x * 4 + 2] = row[x * 3 + 2]; d[x * 4 + 3] = 255; } }
  free(row); jpeg_finish_decompress(&c); jpeg_destroy_decompress(&c); return 0;
}
// ---------- PNG ----------
static int png_decode(const uint8_t *in, size_t n, Bitmap *b) {
  png_image im; memset(&im, 0, sizeof im); im.version = PNG_IMAGE_VERSION; if (!png_image_begin_read_from_memory(&im, in, n)) return -1;
  im.format = PNG_FORMAT_RGBA; b->w = im.width; b->h = im.height; b->alpha = 1; b->px = malloc(PNG_IMAGE_SIZE(im)); if (!b->px) { png_image_free(&im); return -1; }
  if (!png_image_finish_read(&im, NULL, b->px, 0, NULL)) { free(b->px); b->px = NULL; png_image_free(&im); return -1; } return 0;
}
// ---------- WebP (anche animato: primo fotogramma) ----------
static int webp_decode(const uint8_t *in, size_t n, Bitmap *b) {
  WebPData d = { in, n }; WebPAnimDecoderOptions o; if (!WebPAnimDecoderOptionsInit(&o)) return -1; o.color_mode = MODE_RGBA; o.use_threads = 0;
  WebPAnimDecoder *dec = WebPAnimDecoderNew(&d, &o); if (!dec) { int w, h; uint8_t *p = WebPDecodeRGBA(in, n, &w, &h); if (!p) return -1; b->px = malloc((size_t)w * h * 4); memcpy(b->px, p, (size_t)w * h * 4); WebPFree(p); b->w = w; b->h = h; b->alpha = 1; return 0; }
  WebPAnimInfo info; WebPAnimDecoderGetInfo(dec, &info); uint8_t *frame; int ts; if (!WebPAnimDecoderGetNext(dec, &frame, &ts)) { WebPAnimDecoderDelete(dec); return -1; }
  b->w = info.canvas_width; b->h = info.canvas_height; b->alpha = 1; b->px = malloc((size_t)b->w * b->h * 4); memcpy(b->px, frame, (size_t)b->w * b->h * 4); WebPAnimDecoderDelete(dec); return 0;
}
static int kind_of(const uint8_t *p, size_t n) { if (n > 3 && p[0] == 0xFF && p[1] == 0xD8) return 1; if (n > 8 && !memcmp(p, "\x89PNG", 4)) return 2; if (n > 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4)) return 3; return 0; }
int img_info(const uint8_t *in, size_t n, ImgInfo *info) {
  int k = kind_of(in, n); if (!k) return -1;
  if (k == 1) { info->fmt = "jpeg"; return jpeg_dims(in, n, &info->w, &info->h); }
  if (k == 2) { png_image im; memset(&im, 0, sizeof im); im.version = PNG_IMAGE_VERSION; if (!png_image_begin_read_from_memory(&im, in, n)) return -1; info->w = im.width; info->h = im.height; info->fmt = "png"; png_image_free(&im); return 0; }
  WebPBitstreamFeatures f; if (WebPGetFeatures(in, n, &f) != VP8_STATUS_OK) return -1; info->w = f.width; info->h = f.height; info->fmt = "webp"; return 0;
}
// ---------- ridimensionamento a media su area (buona qualita' in riduzione) ----------
static void resize_area(const Bitmap *s, Bitmap *d, int dw, int dh, int cx, int cy, int cw, int ch) {   // ritaglia (cx,cy,cw,ch) dalla sorgente e porta a dw x dh
  d->w = dw; d->h = dh; d->alpha = s->alpha; d->px = malloc((size_t)dw * dh * 4); if (!d->px) return;
  for (int y = 0; y < dh; y++) {
    int y0 = cy + (int)((long long)y * ch / dh), y1 = cy + (int)((long long)(y + 1) * ch / dh); if (y1 <= y0) y1 = y0 + 1; if (y1 > s->h) y1 = s->h;
    for (int x = 0; x < dw; x++) {
      int x0 = cx + (int)((long long)x * cw / dw), x1 = cx + (int)((long long)(x + 1) * cw / dw); if (x1 <= x0) x1 = x0 + 1; if (x1 > s->w) x1 = s->w;
      unsigned long r = 0, g = 0, b = 0, a = 0, cnt = 0, ra = 0;
      for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) { const uint8_t *p = s->px + ((size_t)yy * s->w + xx) * 4; ra += p[3]; r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; cnt++; }
      uint8_t *o = d->px + ((size_t)y * dw + x) * 4;
      if (a) { o[0] = r / a; o[1] = g / a; o[2] = b / a; } else { o[0] = o[1] = o[2] = 0; } o[3] = cnt ? ra / cnt : 255;
    }
  }
}
static void orient(Bitmap *b, int o) {   // applica l'orientamento EXIF (2..8)
  if (o <= 1 || o > 8) return; int w = b->w, h = b->h; int nw = (o >= 5) ? h : w, nh = (o >= 5) ? w : h; uint8_t *n = malloc((size_t)nw * nh * 4); if (!n) return;
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int nx, ny; switch (o) { case 2: nx = w - 1 - x; ny = y; break; case 3: nx = w - 1 - x; ny = h - 1 - y; break; case 4: nx = x; ny = h - 1 - y; break; case 5: nx = y; ny = x; break; case 6: nx = h - 1 - y; ny = x; break; case 7: nx = h - 1 - y; ny = w - 1 - x; break; default: nx = y; ny = w - 1 - x; break; } memcpy(n + ((size_t)ny * nw + nx) * 4, b->px + ((size_t)y * w + x) * 4, 4); }
  free(b->px); b->px = n; b->w = nw; b->h = nh;
}
typedef struct { uint8_t *d; size_t n, cap; } OutBuf;
static void ob_add(OutBuf *o, const void *p, size_t n) { if (o->n + n > o->cap) { o->cap = (o->n + n) * 2 + 4096; o->d = realloc(o->d, o->cap); } memcpy(o->d + o->n, p, n); o->n += n; }
static int jpeg_encode(const Bitmap *b, int q, uint8_t **out, size_t *outn) {
  struct jpeg_compress_struct c; JErr je; c.err = jpeg_std_error(&je.pub); je.pub.error_exit = jerr_exit; unsigned char *buf = NULL; unsigned long len = 0; uint8_t *row = NULL;
  if (setjmp(je.jb)) { jpeg_destroy_compress(&c); free(buf); free(row); return -1; }
  jpeg_create_compress(&c); jpeg_mem_dest(&c, &buf, &len); c.image_width = b->w; c.image_height = b->h; c.input_components = 3; c.in_color_space = JCS_RGB; jpeg_set_defaults(&c); jpeg_set_quality(&c, q, TRUE); jpeg_start_compress(&c, TRUE);
  row = malloc((size_t)b->w * 3);
  while (c.next_scanline < c.image_height) { const uint8_t *s = b->px + (size_t)c.next_scanline * b->w * 4; for (int x = 0; x < b->w; x++) { int a = s[x * 4 + 3]; row[x * 3] = (s[x * 4] * a + 255 * (255 - a)) / 255; row[x * 3 + 1] = (s[x * 4 + 1] * a + 255 * (255 - a)) / 255; row[x * 3 + 2] = (s[x * 4 + 2] * a + 255 * (255 - a)) / 255; } uint8_t *rp = row; jpeg_write_scanlines(&c, &rp, 1); }
  free(row); jpeg_finish_compress(&c); jpeg_destroy_compress(&c); *out = malloc(len); memcpy(*out, buf, len); *outn = len; free(buf); return 0;
}
static int png_encode(const Bitmap *b, uint8_t **out, size_t *outn) {
  png_image im; memset(&im, 0, sizeof im); im.version = PNG_IMAGE_VERSION; im.width = b->w; im.height = b->h; im.format = PNG_FORMAT_RGBA;
  png_alloc_size_t sz = 0; if (!png_image_write_to_memory(&im, NULL, &sz, 0, b->px, 0, NULL)) return -1; *out = malloc(sz); if (!png_image_write_to_memory(&im, *out, &sz, 0, b->px, 0, NULL)) { free(*out); return -1; } *outn = sz; return 0;
}
int img_convert(const uint8_t *in, size_t n, int max_w, int max_h, int fit, int out_fmt, int quality, int rotate, uint8_t **out, size_t *outn, int *ow, int *oh) {
  int k = kind_of(in, n); if (!k) return -1; Bitmap s = { 0 }; int rc = -1; int want_w = max_w > 0 ? max_w : 0, want_h = max_h > 0 ? max_h : 0;
  int orient_v = (k == 1 && rotate) ? exif_orientation(in, n) : 1;
  if (k == 1) { int tw = want_w, th = want_h; if (orient_v >= 5) { int t = tw; tw = th; th = t; } rc = jpeg_decode(in, n, tw ? tw : 1, th ? th : 1, &s); } else if (k == 2) rc = png_decode(in, n, &s); else rc = webp_decode(in, n, &s);
  if (rc != 0 || !s.px) return -1;
  if (orient_v > 1) orient(&s, orient_v);
  int dw = s.w, dh = s.h, cx = 0, cy = 0, cw = s.w, ch = s.h;
  if (fit == 1 && max_w > 0 && max_h > 0) {   // cover: ritaglio centrale con le proporzioni richieste
    if ((long long)s.w * max_h > (long long)s.h * max_w) { cw = (int)((long long)s.h * max_w / max_h); cx = (s.w - cw) / 2; } else { ch = (int)((long long)s.w * max_h / max_w); cy = (s.h - ch) / 2; }
    dw = max_w; dh = max_h; if (cw < dw) { dw = cw; dh = ch < max_h ? ch : (int)((long long)cw * max_h / max_w); }
  } else {
    double sc = 1.0; if (max_w > 0 && s.w > max_w) sc = (double)max_w / s.w; if (max_h > 0 && s.h * sc > max_h) sc = (double)max_h / s.h; if (max_w <= 0 && max_h > 0 && s.h > max_h) sc = (double)max_h / s.h;
    if (sc < 1.0) { dw = (int)(s.w * sc + 0.5); dh = (int)(s.h * sc + 0.5); if (dw < 1) dw = 1; if (dh < 1) dh = 1; }
  }
  Bitmap d = { 0 }; if (dw == s.w && dh == s.h && cx == 0 && cy == 0) { d = s; s.px = NULL; } else { resize_area(&s, &d, dw, dh, cx, cy, cw, ch); bm_free(&s); if (!d.px) return -1; }
  rc = out_fmt == 1 ? png_encode(&d, out, outn) : jpeg_encode(&d, quality > 0 ? quality : 80, out, outn); if (ow) *ow = d.w; if (oh) *oh = d.h; bm_free(&d); return rc;
}
