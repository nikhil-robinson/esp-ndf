/* P4 H.264 encoder (h264.h). Written from the ESP32-P4 TRM chapter 41 (41.5.1 ENC_CORE, 41.5.2 H264_DMA incl. the channel
 * configuration list, 41.7.1 GOP mode procedure) with registers from the generated regs/h264.h and regs/h264_dma.h: there is no
 * ESP-IDF HAL for this block. Buffer geometry and the order of the DMA starts are the TRM's; the points the TRM leaves open are
 * marked "ASSUMPTION" and are the first things to check on the board. NOT VERIFIED ON HARDWARE. */
#include "h264.h"
#include "clk.h"
#include "cpu.h"
#include "spin.h"
#include "soc.h"
#include "cache_p4.h"
#include "dma2d.h"
#include "regs/h264.h"
#include "regs/h264_dma.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"
#include <string.h>

#define E(F, v)   FPUT(H264_##F, v)
#define D(F, v)   FPUT(H264_DMA_##F, v)
#define EV_DB_TMP   BIT(0)
#define EV_REC      BIT(1)
#define EV_FRAME    BIT(2)
#define EV_MOVE     BIT(3)      /* DMA_MOVE_2MB_LINE_DONE */
#define TXR(n, r)   (H264_DMA_OUT_##r##_CH0_REG + 0x100 * (n))
#define RXR(n, r)   (H264_DMA_IN_##r##_CH0_REG + 0x100 * (n))
static const uint8_t pbyte_of[5] = { 4, 3, 4, 3, 2 };            /* ORI_PBYTE: 3 B, 2 B, 3 B, 2 B, 1.5 B */

static struct {
    h264_cfg_t c; int mbx, mby; bool up; int frame; int cur;      /* cur: which of the two reference sets is written */
    uint8_t *ref12[2], *ref4[2], *mv, *dbtmp, *refin;
    uint32_t sz12, sz4, szdb, szref;
    dma2d_desc_t d[11] __attribute__((aligned(64)));              /* TX0..4, RX0..4 (+ RX5 has no descriptors) */
} g;

enum { TX0, TX1, TX2, TX3, TX4, RX0, RX1, RX2, RX3, RX4 };

static void geom(int w, int h, uint32_t *s12, uint32_t *s4, uint32_t *sdb, uint32_t *sref)
{
    uint32_t mx = w / 16, my = h / 16;
    *s12 = mx * (my - 1) * (16 * 12 + 8 * 4 * 2) + mx * 16 * 16 * 3 / 2;     /* TX3 / RX0 space */
    *s4 = mx * (my - 1) * (16 * 4 + 8 * 4 * 2);                              /* TX4 / RX1 space */
    *sdb = mx * (16 * 4 + 8 * 4 * 2);                                        /* TX2 / RX2: one MB line of deblocking intermediate data */
    *sref = mx * 256 * 3;                                                    /* TX1: 3 x (NUM_H_MB x 16 x 16) */
}

size_t h264_work_size(int w, int h)
{
    uint32_t a, b, c, d; geom(w, h, &a, &b, &c, &d);
    uint32_t mbs = (w / 16) * (h / 16);
    return 2 * (((a + 63) & ~63u) + ((b + 63) & ~63u)) + ((mbs * 4 + 63) & ~63u) + ((c + 63) & ~63u) + ((d + 63) & ~63u) + 64;
}

/* ---- bit writer for the NAL headers / SPS / PPS / slice header ---- */
typedef struct { uint8_t *p; size_t n, cap; uint32_t acc; int bits; } bw_t;
static void put(bw_t *b, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        b->acc = b->acc << 1 | (v >> i & 1);
        if (++b->bits == 8) { if (b->n < b->cap) b->p[b->n] = (uint8_t)b->acc; b->n++; b->acc = 0; b->bits = 0; }
    }
}
static void ue(bw_t *b, uint32_t v) { int n = 0; uint32_t x = v + 1; while (x >> (n + 1)) n++; put(b, 0, n); put(b, v + 1, n + 1); }
static void se(bw_t *b, int v) { ue(b, v > 0 ? 2 * v - 1 : -2 * v); }
static void trail(bw_t *b) { put(b, 1, 1); while (b->bits) put(b, 0, 1); }

static void emit_nal(bw_t *dst, const uint8_t *rbsp, size_t n, int ref_idc, int type)   /* start code + header + RBSP with emulation prevention */
{
    uint8_t sc[5] = { 0, 0, 0, 1, (uint8_t)(ref_idc << 5 | type) };
    for (int i = 0; i < 5; i++) put(dst, sc[i], 8);
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && rbsp[i] <= 3) { put(dst, 3, 8); zeros = 0; }
        put(dst, rbsp[i], 8);
        zeros = rbsp[i] == 0 ? zeros + 1 : 0;
    }
}

static size_t make_sps_pps(uint8_t *out, size_t cap)
{
    uint8_t tmp[64]; bw_t o = { out, 0, cap, 0, 0 }, b = { tmp, 0, sizeof tmp, 0, 0 };
    put(&b, 66, 8); put(&b, 0xC0, 8); put(&b, 40, 8);                       /* baseline, constraint set 0 + 1, level 4.0 */
    ue(&b, 0);                                                              /* sps id */
    ue(&b, 4);                                                              /* log2_max_frame_num - 4 -> 8 bits */
    ue(&b, 2);                                                              /* pic_order_cnt_type 2: output order = decoding order */
    ue(&b, 1);                                                              /* one reference frame */
    put(&b, 0, 1);                                                          /* gaps not allowed */
    ue(&b, g.mbx - 1); ue(&b, g.mby - 1);
    put(&b, 1, 1);                                                          /* frame_mbs_only */
    put(&b, 1, 1);                                                          /* direct_8x8_inference */
    put(&b, 0, 1); put(&b, 0, 1);                                           /* no cropping, no VUI */
    trail(&b);
    emit_nal(&o, tmp, b.n, 3, 7);
    bw_t p = { tmp, 0, sizeof tmp, 0, 0 };
    ue(&p, 0); ue(&p, 0);                                                   /* pps id, sps id */
    put(&p, 0, 1); put(&p, 0, 1);                                           /* CAVLC, no bottom-field pic order */
    ue(&p, 0);                                                              /* one slice group */
    ue(&p, 0); ue(&p, 0);                                                   /* ref idx defaults */
    put(&p, 0, 1); put(&p, 0, 2);                                           /* no weighted prediction */
    se(&p, 0); se(&p, 0); se(&p, 0);                                        /* pic init qp 26, qs, chroma qp offset */
    put(&p, 1, 1);                                                          /* deblocking filter control present */
    put(&p, 0, 1); put(&p, 0, 1);                                           /* constrained intra off, redundant pic cnt off */
    trail(&p);
    emit_nal(&o, tmp, p.n, 3, 8);
    return o.n;
}

/* slice header bits -> SLICE_BYTE_MSB / LSB / REMAIN registers (first bit = MSB of the MSB word, TRM 41.7.1 step 2f) */
static void slice_header(bool idr, int frame_num)
{
    uint8_t buf[16]; bw_t b = { buf, 0, sizeof buf, 0, 0 };
    ue(&b, 0);                                                              /* first_mb_in_slice */
    ue(&b, idr ? 7 : 5);                                                    /* slice_type: I / P (all slices of the picture) */
    ue(&b, 0);                                                              /* pps id */
    put(&b, frame_num & 0xFF, 8);                                           /* frame_num */
    if (idr) ue(&b, 0);                                                     /* idr_pic_id */
    if (!idr) { put(&b, 0, 1); put(&b, 0, 1); }                             /* num_ref_idx_override off, ref_pic_list_modification_flag_l0 */
    if (idr) { put(&b, 0, 1); put(&b, 0, 1); } else put(&b, 0, 1);          /* dec_ref_pic_marking: no_output_of_prior / long_term, or adaptive off */
    se(&b, g.c.qp - 26);                                                    /* slice_qp_delta */
    ue(&b, g.c.deblock ? 0 : 1);                                            /* disable_deblocking_filter_idc */
    if (g.c.deblock) { se(&b, 0); se(&b, 0); }
    int nbits = (int)b.n * 8 + b.bits, nbytes = nbits / 8, rem = nbits % 8;
    uint32_t rest = b.bits ? b.acc : 0;                                     /* the bits that did not fill a byte, right-aligned */
    uint32_t msb = 0, lsb = 0;
    for (int i = 0; i < nbytes; i++) { if (i < 4) msb |= (uint32_t)buf[i] << (24 - 8 * i); else lsb |= (uint32_t)buf[i] << (8 * (nbytes - 1 - i)); }
    E(SLICE_BYTE_MSB, msb); E(SLICE_BYTE_LSB, lsb);
    E(SLICE_REMAIN_BIT, rest); E(SLICE_REMAIN_BITLENGTH, rem); E(SLICE_BYTE_LENGTH, nbytes);
}

/* ---- DMA channel helpers (same engine as dma2d.h; descriptors circular, 1-D unless noted) ---- */
static void tx_reset(int n)
{
    FPUT(H264_DMA_OUT_CMD_DISABLE_CH0, 0);
    uint32_t c = TXR(n, CONF0);
    REG_SET(c, BIT(H264_DMA_OUT_CMD_DISABLE_CH0_S));
    SPIN_US(!(REG(TXR(n, STATE)) & BIT(H264_DMA_OUT_RESET_AVAIL_CH0_S)), 1000);
    REG_SET(c, BIT(H264_DMA_OUT_RST_CH0_S)); REG_CLR(c, BIT(H264_DMA_OUT_RST_CH0_S)); REG_CLR(c, BIT(H264_DMA_OUT_CMD_DISABLE_CH0_S));
}
static void rx_reset(int n)
{
    uint32_t c = RXR(n, CONF0);
    REG_SET(c, BIT(H264_DMA_IN_CMD_DISABLE_CH0_S));
    SPIN_US(!(REG(RXR(n, STATE)) & BIT(H264_DMA_IN_RESET_AVAIL_CH0_S)), 1000);
    REG_SET(c, BIT(H264_DMA_IN_RST_CH0_S)); REG_CLR(c, BIT(H264_DMA_IN_RST_CH0_S)); REG_CLR(c, BIT(H264_DMA_IN_CMD_DISABLE_CH0_S));
}
static void tx_start(int n, dma2d_desc_t *d)
{
    uint32_t c = TXR(n, CONF0);
    REG_FIELD(c, H264_DMA_OUT_MEM_BURST_LENGTH_CH0_S, 3, n == 2 ? 3 : 4);       /* burst lengths: TX2 and RX2 equal; 128 bytes elsewhere */
    REG_SET(c, BIT(H264_DMA_OUT_PAGE_BOUND_EN_CH0_S) | BIT(H264_DMA_OUT_EOF_MODE_CH0_S));
    REG_CLR(c, BIT(H264_DMA_OUT_CHECK_OWNER_CH0_S) | BIT(H264_DMA_OUT_AUTO_WRBACK_CH0_S));
    if (n == 0) REG_SET(c, BIT(H264_DMA_OUT_REORDER_EN_CH0_S));                 /* HB = 64 != 16 */
    cache_dma_writeback(d, sizeof *d);
    REG(TXR(n, LINK_ADDR)) = (uint32_t)(uintptr_t)d;
    REG_SET(TXR(n, LINK_CONF), BIT(H264_DMA_OUTLINK_START_CH0_S));
}
static void rx_start(int n, dma2d_desc_t *d)
{
    uint32_t c = RXR(n, CONF0);
    REG_FIELD(c, H264_DMA_IN_MEM_BURST_LENGTH_CH0_S, 3, n == 2 ? 3 : 4);
    REG_SET(c, BIT(H264_DMA_IN_PAGE_BOUND_EN_CH0_S));
    REG_CLR(c, BIT(H264_DMA_IN_CHECK_OWNER_CH0_S));
    cache_dma_wbinv(d, sizeof *d);
    REG(RXR(n, LINK_ADDR)) = (uint32_t)(uintptr_t)d;
    REG_SET(RXR(n, LINK_CONF), BIT(H264_DMA_INLINK_START_CH0_S));
}

static void ring(dma2d_desc_t *d, void *buf, uint32_t size, bool tx)         /* circular 1-D descriptor over `buf` */
{
    dma2d_desc_1d(d, buf, size & 0x0FFFFFFF, tx ? size : 0, false, d);
}

int h264_init(const h264_cfg_t *c)
{
    if (!c || c->w < 32 || c->h < 32 || (c->w | c->h) & 15 || c->w > 2032 || c->h > 2032 || c->qp < 10 || c->qp > 51 || (unsigned)c->color > H264_YUV420 ||
        !c->work || ((uintptr_t)c->work & 63) || c->work_size < h264_work_size(c->w, c->h)) return -1;
    g.c = *c; g.mbx = c->w / 16; g.mby = c->h / 16;
    geom(c->w, c->h, &g.sz12, &g.sz4, &g.szdb, &g.szref);
    uint8_t *p = c->work;
    for (int i = 0; i < 2; i++) { g.ref12[i] = p; p += (g.sz12 + 63) & ~63u; g.ref4[i] = p; p += (g.sz4 + 63) & ~63u; }
    g.mv = p; p += ((uint32_t)g.mbx * g.mby * 4 + 63) & ~63u;
    g.dbtmp = p; p += (g.szdb + 63) & ~63u;
    g.refin = p;
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_H264_SYS_CLK_EN);
    FPUT(HP_SYS_CLKRST_REG_H264_CLK_SRC_SEL, 1); FPUT(HP_SYS_CLKRST_REG_H264_CLK_DIV_NUM, 0);     /* 240 MHz */
    FSET(HP_SYS_CLKRST_REG_REF_240M_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_H264_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_H264); FCLR(HP_SYS_CLKRST_REG_RST_EN_H264);
    irq_unlock(t);
    REG(H264_CONF_REG) = 0x0FFFFFFFu;                                          /* core clock + every RAM / module clock gate on */
    REG(H264_DMA_RST_CONF_REG) = BIT(H264_DMA_CLK_EN_S);
    REG_SET(H264_DMA_RST_CONF_REG, 0xF); REG_CLR(H264_DMA_RST_CONF_REG, 0xF);   /* AXI master FIFOs */
    for (int i = 0; i < 5; i++) tx_reset(i);
    for (int i = 0; i < 6; i++) rx_reset(i);
    g.up = true; g.frame = 0; g.cur = 0;
    return 0;
}

void h264_reset_gop(void) { g.frame = 0; }

void h264_rc(bool on, int qmin, int qmax, int u)
{
    E(A_MB_RATE_CTRL_EN, on); E(A_QP_MIN, qmin); E(A_QP_MAX, qmax); E(A_RATE_CTRL_U, u);
}

static int wait_ev(uint32_t ev, uint32_t us)
{
    SPIN_US(!(REG(H264_INT_RAW_REG) & ev), us);
    int ok = (REG(H264_INT_RAW_REG) & ev) ? 0 : -1;
    REG(H264_INT_CLR_REG) = ev;
    return ok;
}

int h264_encode(const void *frame, void *out, size_t cap)
{
    if (!g.up || !frame || !out || ((uintptr_t)frame & 63) || ((uintptr_t)out & 63) || cap < (size_t)g.c.w * g.c.h * 3 / 2 + 256) return -1;
    bool idr = g.frame == 0;
    dma2d_desc_t *d = g.d;
    uint32_t w = g.c.w, h = g.c.h, mbx = g.mbx;
    size_t hdr = 0;
    uint8_t *o = out;
    if (idr) hdr = make_sps_pps(o, cap);
    /* slice NAL header; the encoder core adds the slice header + data after it (ASSUMPTION: emulation prevention and the stop bit
     * are produced by the core's bit-stream stage) */
    uint8_t *bs = o + hdr;
    bs[0] = 0; bs[1] = 0; bs[2] = 0; bs[3] = 1; bs[4] = (uint8_t)((idr ? 3 : 2) << 5 | (idr ? 5 : 1));
    size_t nal_hdr = 5;
    cache_dma_writeback(bs, 64);
    int cur = g.cur, prev = cur ^ 1;
    /* ---- ENC_CORE ---- */
    E(FRAME_MODE, 0); E(DUAL_STREAM_MODE, 0); E(GOP_NUM, g.c.gop);
    E(A_SYS_TOTAL_MB_X, g.mbx); E(A_SYS_TOTAL_MB_Y, g.mby);
    E(A_QP, g.c.qp); E(A_BYPASS_DB_FILTER, !g.c.deblock);
    E(A_ORI_COLOR_SPACE, g.c.color);
    D(ORI_PBYTE, pbyte_of[g.c.color]);
    slice_header(idr, g.frame);
    REG(H264_INT_ENA_REG) = EV_DB_TMP | EV_REC | EV_FRAME | EV_MOVE;
    REG(H264_INT_CLR_REG) = 0x1F;
    cache_dma_wbinv(g.dbtmp, g.szdb);
    if (idr) {
        /* descriptors: TX0 original picture (2-D, 64 x 16 blocks, reorder), TX1 reference rows from the internal buffer, TX2 DB temp
         * read, TX3 / TX4 reference read from external memory, RX0 / RX1 reference write, RX2 DB temp write, RX4 bit stream */
        cache_dma_writeback(frame, (size_t)w * h * 4);
        dma2d_desc_2d(&d[TX0], (void *)frame, w, h, 64, 16, 0, 0, pbyte_of[g.c.color], true, false, &d[TX0]);
        dma2d_desc_2d(&d[TX1], g.refin, g.mbx * 256, 3, 256, 3, 0, 0, DMA2D_PBYTE_1B0, true, false, &d[TX1]);
        ring(&d[TX2], g.dbtmp, g.szdb, true);
        ring(&d[TX3], g.ref12[prev], g.sz12, true);
        ring(&d[TX4], g.ref4[prev], g.sz4, true);
        ring(&d[RX0], g.ref12[cur], g.sz12, false);
        ring(&d[RX1], g.ref4[cur], g.sz4, false);
        ring(&d[RX2], g.dbtmp, g.szdb, false);
        dma2d_desc_1d(&d[RX4], bs + nal_hdr, (uint32_t)((size_t)w * h * 3 / 2), 0, false, &d[RX4]);
        rx_reset(0); rx_reset(1); rx_reset(2); rx_reset(4);
        tx_reset(0);
        REG(H264_DMA_COUNTER_RST_REG) = 0xF; REG(H264_DMA_COUNTER_RST_REG) = 0;
        D(BLOCK_START_ADDR_CH5, (uint32_t)(uintptr_t)g.refin);
        D(BLOCK_ROW_LENGTH_4LINE_CH5, mbx * (16 * 4 + 64)); D(BLOCK_ROW_LENGTH_12LINE_CH5, mbx * (16 * 12 + 64));
        D(BLOCK_LENGTH_4LINE_CH5, 16 * 4 + 64); D(BLOCK_LENGTH_12LINE_CH5, 16 * 12 + 64);
        tx_start(TX0, &d[TX0]);
        rx_start(0, &d[RX0]); rx_start(1, &d[RX1]);
        rx_start(2, &d[RX2]);
        rx_start(4, &d[RX4]);
    } else {
        rx_start(2, &d[RX2]);
        dma2d_desc_2d(&d[TX0], (void *)frame, w, h, 64, 16, 0, 0, pbyte_of[g.c.color], true, false, &d[TX0]);
        cache_dma_writeback(frame, (size_t)w * h * 4);
        tx_reset(0); tx_start(TX0, &d[TX0]);
        dma2d_desc_1d(&d[RX4], bs + nal_hdr, (uint32_t)((size_t)w * h * 3 / 2), 0, false, &d[RX4]);   /* ASSUMPTION: the bit-stream ring is re-pointed per frame */
        rx_reset(4); rx_start(4, &d[RX4]);
    }
    REG_SET(H264_SYS_CTRL_REG, BIT(H264_FRAME_START_S));
    int r = wait_ev(EV_DB_TMP, 1000000);
    if (!r) tx_start(TX2, &d[TX2]);
    if (!r && idr) {
        r = wait_ev(EV_REC, 1000000);
        if (!r) {
            /* RX5 (reference to internal memory), TX3 / TX4 (external reference reads); then move two MB lines and start TX1 */
            REG_SET(RXR(5, CONF0), BIT(H264_DMA_IN_PAGE_BOUND_EN_CH5_S));
            tx_start(TX3, &d[TX3]); tx_start(TX4, &d[TX4]);
            REG_SET(H264_SYS_CTRL_REG, BIT(H264_DMA_MOVE_START_S));
            r = wait_ev(EV_MOVE, 1000000);
            if (!r) tx_start(TX1, &d[TX1]);
        }
    }
    if (!r) r = wait_ev(EV_FRAME, 2000000);
    uint32_t len = REG(H264_FRAME_CODE_LENGTH_REG);
    tx_reset(2); rx_reset(2);
    REG(H264_DMA_COUNTER_RST_REG) = BIT(H264_DMA_RX_CH2_INTER_COUNTER_RST_S); REG(H264_DMA_COUNTER_RST_REG) = 0;
    cache_dma_wbinv(bs + nal_hdr, len ? len : 64);
    if (r || len == 0 || hdr + nal_hdr + len > cap) return -1;
    g.cur ^= 1; g.frame++;
    if (g.c.gop && g.frame >= g.c.gop) g.frame = 0;
    return (int)(hdr + nal_hdr + len);
}
