/* JPEG codec (ESP32-P4 only, TRM chapter 37): baseline JPEG encoder (RGB888 / RGB565 / GRAY / YUV422 / YUV444 / YUV420
 * input, 4:4:4 / 4:2:2 / 4:2:0 / gray output) on the 2D-DMA (dma2d.h). The hardware produces the entropy-coded scan; this
 * driver writes the JFIF headers (standard Annex K tables, quality-scaled quantisation tables) in front of it.
 * Buffers 64-byte aligned (cache lines), PSRAM or internal RAM. */
#pragma once
#include "ndf.h"

typedef enum { JPEG_IN_RGB888, JPEG_IN_RGB565, JPEG_IN_GRAY, JPEG_IN_YUV422, JPEG_IN_YUV444, JPEG_IN_YUV420 } jpeg_in_t;
typedef enum { JPEG_SUB_444, JPEG_SUB_422, JPEG_SUB_420, JPEG_SUB_GRAY } jpeg_sub_t;

/* Encode w x h pixels at `in` to `out` (capacity out_cap). quality 1..100. Returns the JPEG size in bytes, or -1.
 * 422 needs YUV422 or RGB input, 420 needs RGB / YUV420 input, gray needs GRAY input (as the hardware allows). */
int jpeg_encode(const void *in, int w, int h, jpeg_in_t fmt, jpeg_sub_t sub, int quality, void *out, size_t out_cap);
