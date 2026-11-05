/* P4 JPEG encoder (jpeg.h). Flow of ESP-IDF v6.1 esp_driver_jpeg/jpeg_encode.c, jpeg_emit_marker.c, jpeg_param.c and
 * esp_hal_jpeg/esp32p4/jpeg_ll.h: software header (SOI, APP0, DQT, SOF0, DHT, COM padding to a cache line, SOS), the
 * hardware's standard Huffman tables, quantisation tables written through the QNR FIFO windows, 2-D DMA read of the picture in
 * MCU rows (TX0 with macro block reorder), 1-D DMA write of the scan behind the header; the EOF descriptor holds the length. */
#include "jpeg.h"
#include "dma2d.h"
#include "clk.h"
#include "cpu.h"
#include "soc.h"
#include "cache_p4.h"
#include "regs/jpeg.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"
#include <string.h>

#define JF(F, v) REG_FIELD(JPEG_##F##_R, JPEG_##F##_S, JPEG_##F##_W, v)
#define LINE 64u

static const uint8_t zz[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                                35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
static const uint8_t lum_q[64] = { 16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
                                   18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99 };
static const uint8_t chr_q[64] = { 17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                   99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99 };
static const uint8_t dc_l_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 }, dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t dc_c_bits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t ac_l_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d }, ac_c_bits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t ac_l_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa };
static const uint8_t ac_c_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa };
/* DMA block width (pixels) per input format x sub-sampling (jpeg_param.c enc_hb_tbl; 0: not supported) */
static const uint8_t hb_tbl[6][4] = { { 40, 32, 32, 0 }, { 64, 64, 48, 0 }, { 0, 0, 0, 128 }, { 0, 64, 0, 0 }, { 40, 0, 0, 0 }, { 0, 0, 48, 0 } };
static const uint8_t fmt_cs[6] = { 0, 2, 3, 1, 0, 0 };                            /* COLOR_SPACE field: RGB888, RGB565, GRAY, YUV422; 444 / 420 use the extended one */
static const uint8_t fmt_pbyte[6] = { DMA2D_PBYTE_3B0, DMA2D_PBYTE_2B0, DMA2D_PBYTE_1B0, DMA2D_PBYTE_2B0, DMA2D_PBYTE_3B0, DMA2D_PBYTE_1B5 };

static bool up;
static dma2d_desc_t dtx __attribute__((aligned(64))), drx __attribute__((aligned(64)));

static int init(void)
{
    if (up) return 0;
    if (dma2d_init()) return -1;
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_JPEG_SYS_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_JPEG); FCLR(HP_SYS_CLKRST_REG_RST_EN_JPEG);
    irq_unlock(t);
    up = true;
    return 0;
}

typedef struct { uint8_t *p; size_t n, cap; } hdr_t;
static void b(hdr_t *h, unsigned v) { if (h->n < h->cap) h->p[h->n] = (uint8_t)v; h->n++; }
static void w16(hdr_t *h, unsigned v) { b(h, v >> 8); b(h, v); }

static void qtab(uint8_t *out, const uint8_t *base, int quality)                 /* jpeg_emit_marker.c compute_quant_table */
{
    int sc = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; i++) { int t = (base[i] * sc + 50) / 100; out[i] = (uint8_t)(t < 1 ? 1 : t > 255 ? 255 : t); }
}

static void dht(hdr_t *h, int tc_th, const uint8_t *bits, const uint8_t *vals, int nv)
{
    w16(h, 0xFFC4); w16(h, 2 + 1 + 16 + nv); b(h, tc_th);
    for (int i = 0; i < 16; i++) b(h, bits[i]);
    for (int i = 0; i < nv; i++) b(h, vals[i]);
}

int jpeg_encode(const void *in, int w, int ht, jpeg_in_t fmt, jpeg_sub_t sub, int quality, void *out, size_t cap)
{
    static const uint8_t mcuy[4] = { 8, 8, 16, 8 };
    if (!in || !out || w < 1 || ht < 1 || w > 0x3FFF || ht > 0x3FFF || (unsigned)fmt > 5 || (unsigned)sub > 3 || quality < 1 || quality > 100 ||
        ((uintptr_t)in | (uintptr_t)out) & (LINE - 1) || cap < 1024 || !hb_tbl[fmt][sub] || init()) return -1;
    if ((sub == JPEG_SUB_GRAY) != (fmt == JPEG_IN_GRAY)) return -1;
    static const uint8_t bpp[6] = { 3, 2, 1, 2, 3, 0 };
    size_t inbytes = fmt == JPEG_IN_YUV420 ? (size_t)w * ht * 3 / 2 : (size_t)w * ht * bpp[fmt];
    uint8_t lq[64], cq[64];
    qtab(lq, lum_q, quality); qtab(cq, chr_q, quality);

    /* ---- headers ---- */
    hdr_t h = { out, 0, cap };
    int ncomp = sub == JPEG_SUB_GRAY ? 1 : 3;
    w16(&h, 0xFFD8);                                                                    /* SOI */
    w16(&h, 0xFFE0); w16(&h, 16); b(&h, 'J'); b(&h, 'F'); b(&h, 'I'); b(&h, 'F'); b(&h, 0); w16(&h, 0x0101); b(&h, 0); w16(&h, 1); w16(&h, 1); b(&h, 0); b(&h, 0);
    for (int t = 0; t < (ncomp == 1 ? 1 : 2); t++) {                                    /* DQT in zig-zag order */
        w16(&h, 0xFFDB); w16(&h, 67); b(&h, t);
        for (int i = 0; i < 64; i++) b(&h, (t ? cq : lq)[zz[i]]);
    }
    w16(&h, 0xFFC0); w16(&h, 8 + 3 * ncomp); b(&h, 8); w16(&h, ht); w16(&h, w); b(&h, ncomp);   /* SOF0 */
    b(&h, 1); b(&h, sub == JPEG_SUB_444 || sub == JPEG_SUB_GRAY ? 0x11 : sub == JPEG_SUB_422 ? 0x21 : 0x22); b(&h, 0);
    if (ncomp == 3) { b(&h, 2); b(&h, 0x11); b(&h, 1); b(&h, 3); b(&h, 0x11); b(&h, 1); }
    dht(&h, 0x00, dc_l_bits, dc_vals, 12); dht(&h, 0x10, ac_l_bits, ac_l_vals, 162);
    if (ncomp == 3) { dht(&h, 0x01, dc_c_bits, dc_vals, 12); dht(&h, 0x11, ac_c_bits, ac_c_vals, 162); }
    /* COM marker pads the header so the scan starts on a cache line (the DMA writes it behind the header) */
    size_t pad = (LINE - ((h.n + 4 + 14) & (LINE - 1))) & (LINE - 1);
    w16(&h, 0xFFFE); w16(&h, 2 + pad + 0); for (size_t i = 0; i < pad; i++) b(&h, 0);
    w16(&h, 0xFFDA); w16(&h, 6 + 2 * ncomp); b(&h, ncomp);                              /* SOS */
    b(&h, 1); b(&h, 0x00); if (ncomp == 3) { b(&h, 2); b(&h, 0x11); b(&h, 3); b(&h, 0x11); }
    b(&h, 0); b(&h, 63); b(&h, 0);
    if (h.n > cap || (h.n & (LINE - 1))) return -1;
    size_t hlen = h.n, payload = cap - hlen;
    cache_dma_writeback(out, hlen);

    /* ---- engine ---- */
    JF(SOFT_RST, 1); JF(SOFT_RST, 0);
    JF(MODE, 0);                                                                         /* encoder */
    JF(COLOR_SPACE, fmt_cs[fmt]);
    if (fmt == JPEG_IN_YUV444 || fmt == JPEG_IN_YUV420) { JF(EXTD_COLOR_SPACE_EN, 1); JF(EXTD_COLOR_SPACE, fmt == JPEG_IN_YUV420); }
    else JF(EXTD_COLOR_SPACE_EN, 0);
    JF(VA, ht); JF(HA, w);
    JF(PIXEL_REV, 0); JF(TAILER_EN, 1); JF(FF_CHECK_EN, 1); JF(QNR_PRECISION, 0);
    JF(QNR_FIFO_EN, 1); JF(LQNR_TBL_SEL, 0); JF(CQNR_TBL_SEL, 1);                       /* quantisation RAM written through the FIFO windows */
    static const uint32_t qreg[4] = { JPEG_T0QNR_REG, JPEG_T1QNR_REG, JPEG_T2QNR_REG, JPEG_T3QNR_REG };
    for (int t = 0; t < 4; t++) {
        REG_FIELD(JPEG_DQT_INFO_REG, 8 * t, 8, t);
        for (int i = 0; i < 64; i++) REG(qreg[t]) = (t & 1 ? cq : lq)[i];
    }
    if (sub != JPEG_SUB_GRAY) JF(SAMPLE_SEL, sub == JPEG_SUB_444 ? 0 : sub == JPEG_SUB_422 ? 1 : 2);
    dma2d_mb_t mb = sub == JPEG_SUB_422 ? DMA2D_MB_8_16 : sub == JPEG_SUB_420 ? DMA2D_MB_16_16 : DMA2D_MB_8_8;
    dma2d_desc_1d(&drx, (uint8_t *)out + hlen, payload & 0x0FFFFFFF, 0, false, 0);        /* 1-D receive; suc_eof written back by the engine */
    drx.w0 |= BIT(31);
    dma2d_desc_2d(&dtx, (void *)in, w, ht, hb_tbl[fmt][sub], mcuy[sub], 0, 0, fmt_pbyte[fmt], true, false, 0);
    cache_dma_writeback(in, inbytes);
    dma2d_cfg_t tx = { .periph = DMA2D_PERIPH_JPEG, .periph_id = 0, .mb = mb, .reorder = true }, rx = { .periph = DMA2D_PERIPH_JPEG, .periph_id = 0 };
    if (dma2d_rx_start(0, &rx, &drx) || dma2d_tx_start(0, &tx, &dtx)) return -1;
    REG(JPEG_INT_CLR_REG) = 0xFFFFFFFFu;
    JF(JPEG_START, 1);
    uint32_t got = 0;
    int r = dma2d_rx_wait(0, 2000000, &got);
    uint32_t err = REG(JPEG_INT_RAW_REG) & (BIT(1) | BIT(16));                           /* RLE parallel error, frame EOF error */
    dma2d_stop(0, 0);
    cache_dma_wbinv((uint8_t *)out + hlen, got ? got : payload);
    return r || err || !got ? -1 : (int)(hlen + got);
}
