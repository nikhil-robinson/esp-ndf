/* H.264 baseline encoder (ESP32-P4 only, TRM chapter 41): hardware CAVLC encoder with its own 11-channel DMA. GOP mode, one
 * slice per frame, intra frame first then P frames, deblocking filter on or off, fixed QP (hardware MB-level rate control
 * available through h264_rc()). The driver writes Annex-B start codes, NAL headers, SPS and PPS and hands the slice header to the
 * encoder core; the encoder produces the slice data. The frame is read as RGB888 / RGB565 / YUV444 / YUV422 / YUV420 (converted to
 * 4:2:0 by the core). Work memory: h264_work_size() bytes, 64-byte aligned, PSRAM or internal RAM (reference pictures,
 * deblocking buffers). Width and height are multiples of 16. */
#pragma once
#include "ndf.h"

typedef enum { H264_RGB888, H264_RGB565, H264_YUV444, H264_YUV422, H264_YUV420 } h264_color_t;
typedef struct {
    int w, h;                     /* pixels, multiples of 16 */
    h264_color_t color;
    int qp;                       /* 10..51 */
    int gop;                      /* frames per GOP: 1 = all intra, 0 = one I frame then P frames forever */
    bool deblock;
    void *work; size_t work_size;
} h264_cfg_t;

size_t h264_work_size(int w, int h);
int    h264_init(const h264_cfg_t *c);
/* encode one frame: returns bytes written to `out` (Annex B: [SPS PPS] slice), or -1. `out` is 64-byte aligned, cap >= w * h * 3 / 2 + 256. */
int    h264_encode(const void *frame, void *out, size_t cap);
void   h264_rc(bool mb_level, int qp_min, int qp_max, int bits_per_mb_row_u);   /* MB level rate control on / off with QP limits and target U */
void   h264_reset_gop(void);                                   /* next frame is an I frame */
