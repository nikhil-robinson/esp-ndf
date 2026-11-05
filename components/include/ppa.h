/* Pixel-Processing Accelerator (ESP32-P4 only, TRM chapter 39): scale / rotate / mirror (SRM) of a picture region, alpha
 * blending of two regions, and solid fills, between pictures in memory (internal RAM or PSRAM) through the 2D-DMA
 * (dma2d.h). Blocking calls. Pixel formats: ARGB8888, RGB888, RGB565, GRAY8; x / y / w / h are in pixels. */
#pragma once
#include "ndf.h"

typedef enum { PPA_ARGB8888, PPA_RGB888, PPA_RGB565, PPA_GRAY8 } ppa_cm_t;
typedef struct {
    void *buf;                  /* picture base address */
    int pic_w, pic_h;           /* the whole picture */
    int x, y, w, h;             /* the region used (SRM input, blend inputs, outputs: offset of the result) */
    ppa_cm_t cm;
} ppa_pic_t;
typedef enum { PPA_ALPHA_KEEP, PPA_ALPHA_FIX, PPA_ALPHA_SCALE, PPA_ALPHA_INVERT } ppa_alpha_t;
typedef struct { ppa_alpha_t mode; uint8_t value; } ppa_a_t;   /* FIX: A = value; SCALE: A = A * value / 256 */

/* out region = in region scaled by scale16 / 16 (16 = 1.0, 1..4095), rotated counter-clockwise by rot (0 / 90 / 180 / 270),
 * mirrored. out.w / out.h are filled in with the size produced. 0 / -1. */
int ppa_srm(const ppa_pic_t *in, ppa_pic_t *out, int scale_x16, int scale_y16, int rot, bool mirror_x, bool mirror_y);
/* out = fg over bg (same region size); alpha handling per input. */
int ppa_blend(const ppa_pic_t *bg, const ppa_pic_t *fg, const ppa_pic_t *out, ppa_a_t bg_a, ppa_a_t fg_a);
/* out region filled with one pixel value (in the format of out.cm: ARGB8888 0xAARRGGBB, RGB888, RGB565, GRAY8) */
int ppa_fill(const ppa_pic_t *out, uint32_t color);
