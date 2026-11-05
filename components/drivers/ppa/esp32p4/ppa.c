/* P4 PPA (ppa.h). Flow of ESP-IDF v6.1 esp_driver_ppa/src/ppa_srm.c / ppa_blend.c / ppa_fill.c and esp_hal_ppa/esp32p4/ppa_ll.h:
 * 2D-DMA descriptors in single-block 2-D mode (the region is a block inside the picture), the engine registers set, then the
 * RX channel's EOF ends the operation. SRM: TX0 (periph id 1) + RX0 (id 1), descriptor-port mode 18x18 for the 16x16 macro
 * block; blend: TX1 (bg, id 2) + TX2 (fg, id 3) + RX1 (id 2); fill: RX1 only. */
#include "ppa.h"
#include "dma2d.h"
#include "clk.h"
#include "cpu.h"
#include "spin.h"
#include "soc.h"
#include "cache_p4.h"
#include "regs/ppa.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"

#define PF(F, v)  REG_FIELD(PPA_##F##_R, PPA_##F##_S, PPA_##F##_W, v)
static dma2d_desc_t dtx[3] __attribute__((aligned(64))), drx __attribute__((aligned(64)));
static bool up;

static const uint8_t cm_id[4] = { 0, 1, 2, 12 };                           /* register code of ARGB8888, RGB888, RGB565, GRAY8 */
static const uint8_t cm_pbyte[4] = { DMA2D_PBYTE_4B0, DMA2D_PBYTE_3B0, DMA2D_PBYTE_2B0, DMA2D_PBYTE_1B0 };
static const uint8_t cm_bytes[4] = { 4, 3, 2, 1 };

static int init(void)
{
    if (up) return 0;
    if (dma2d_init()) return -1;
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_PPA_SYS_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_PPA); FCLR(HP_SYS_CLKRST_REG_RST_EN_PPA);
    irq_unlock(t);
    REG_SET(PPA_REG_CONF_REG, BIT(PPA_CLK_EN_S));
    REG_CLR(PPA_SR_MEM_PD_REG, BIT(PPA_SR_MEM_FORCE_PD_S) | BIT(PPA_SR_MEM_FORCE_PU_S));       /* memory power by the PMU */
    up = true;
    return 0;
}

static bool ok(const ppa_pic_t *p)
{
    return p && p->buf && p->w > 0 && p->h > 0 && p->x >= 0 && p->y >= 0 && p->x + p->w <= p->pic_w && p->y + p->h <= p->pic_h && (unsigned)p->cm < 4 &&
           p->pic_w <= 0x3FFF && p->pic_h <= 0x3FFF;
}
static void wb(const ppa_pic_t *p) { cache_dma_writeback(p->buf, (size_t)p->pic_w * p->pic_h * cm_bytes[p->cm]); }
static void inv(const ppa_pic_t *p) { cache_dma_wbinv(p->buf, (size_t)p->pic_w * p->pic_h * cm_bytes[p->cm]); }

static void desc_in(dma2d_desc_t *d, const ppa_pic_t *p) { dma2d_desc_2d(d, p->buf, p->pic_w, p->pic_h, p->w, p->h, p->x, p->y, cm_pbyte[p->cm], false, true, 0); }
static void desc_out(dma2d_desc_t *d, const ppa_pic_t *p, int hb, int vb) { dma2d_desc_2d(d, p->buf, p->pic_w, p->pic_h, hb, vb, p->x, p->y, cm_pbyte[p->cm], false, true, 0); }

static void alpha_sr(ppa_a_t a)
{
    PF(SR_RX_ALPHA_MOD, a.mode == PPA_ALPHA_FIX ? 1 : a.mode == PPA_ALPHA_SCALE ? 2 : 0);
    PF(SR_RX_ALPHA_INV, a.mode == PPA_ALPHA_INVERT);
    if (a.mode == PPA_ALPHA_FIX || a.mode == PPA_ALPHA_SCALE) PF(SR_RX_FIX_ALPHA, a.value);
}

int ppa_srm(const ppa_pic_t *in, ppa_pic_t *out, int sx16, int sy16, int rot, bool mx, bool my)
{
    if (!ok(in) || !out || !out->buf || (unsigned)out->cm >= 4 || sx16 < 1 || sy16 < 1 || sx16 >= 256 * 16 || sy16 >= 256 * 16 || rot % 90 || (unsigned)rot > 270 || init()) return -1;
    int bw = in->w * sx16 / 16, bh = in->h * sy16 / 16;                     /* the scaled block, before rotation */
    if (rot == 90 || rot == 270) { int t = bw; bw = bh; bh = t; }
    out->w = bw; out->h = bh;
    if (bw < 1 || bh < 1 || out->x + bw > out->pic_w || out->y + bh > out->pic_h) return -1;
    REG_SET(PPA_SR_SCAL_ROTATE_REG, BIT(PPA_SCAL_ROTATE_RST_S)); REG_CLR(PPA_SR_SCAL_ROTATE_REG, BIT(PPA_SCAL_ROTATE_RST_S));
    desc_in(&dtx[0], in);
    desc_out(&drx, out, 2, 2);                                              /* hb / vb are written back by the DMA, any even value */
    wb(in); inv(out);                                                       /* clean the output's lines before the DMA writes it */
    dma2d_cfg_t tx = { .periph = DMA2D_PERIPH_PPA_SRM, .periph_id = 1, .dscr_port = true, .dscr_port_h = 18, .dscr_port_v = 18 };
    dma2d_cfg_t rx = { .periph = DMA2D_PERIPH_PPA_SRM, .periph_id = 1 };
    if (dma2d_rx_start(0, &rx, &drx) || dma2d_tx_start(0, &tx, &dtx[0])) return -1;
    PF(SR_RX_CM, cm_id[in->cm]); PF(SR_TX_CM, cm_id[out->cm]);
    PF(SR_RX_BYTE_SWAP_EN, 0); PF(SR_RX_RGB_SWAP_EN, 0);
    REG_SET(PPA_SR_BYTE_ORDER_REG, BIT(PPA_SR_BK_SIZE_SEL_S));              /* 16x16 macro blocks */
    alpha_sr((ppa_a_t){ PPA_ALPHA_KEEP, 0 });
    PF(SR_ROTATE_ANGLE, rot / 90);
    PF(SR_SCAL_X_INT, sx16 >> 4); PF(SR_SCAL_X_FRAG, sx16 & 15);
    PF(SR_SCAL_Y_INT, sy16 >> 4); PF(SR_SCAL_Y_FRAG, sy16 & 15);
    PF(SR_MIRROR_X, mx); PF(SR_MIRROR_Y, my);
    /* DIG-734 workaround as in ppa_srm.c: bypass the macro block reorder when the block is cut into several units and the
     * last unit is small */
    uint32_t wdiv = out->cm <= PPA_RGB888 ? 32 : 64, wl = (uint32_t)bw % wdiv; wl = wl ? wl : wdiv;
    uint32_t hl = (uint32_t)in->h % 16; hl = hl ? hl : 16;
    uint32_t hout = hl * sy16 / 16;
    PF(SR_MACRO_BK_RO_BYPASS, ((uint32_t)bw > wdiv || in->h > 16) && wl * hout * cm_bytes[out->cm] * 8 < 12 * 128);
    REG(PPA_INT_CLR_REG) = BIT(PPA_SR_EOF_INT_CLR_S) | BIT(PPA_SR_PARAM_CFG_ERR_INT_CLR_S);
    REG_SET(PPA_SR_SCAL_ROTATE_REG, BIT(PPA_SCAL_ROTATE_START_S));
    int r = dma2d_rx_wait(0, 1000000, 0);
    r |= (REG(PPA_SR_PARAM_ERR_ST_REG) != 0) ? -1 : 0;
    dma2d_stop(0, 0);
    inv(out);
    return r ? -1 : 0;
}

static void alpha_bl(bool fg, ppa_a_t a)
{
    if (fg) { PF(BLEND1_RX_ALPHA_MOD, a.mode == PPA_ALPHA_FIX ? 1 : a.mode == PPA_ALPHA_SCALE ? 2 : 0); PF(BLEND1_RX_ALPHA_INV, a.mode == PPA_ALPHA_INVERT);
              if (a.mode == PPA_ALPHA_FIX || a.mode == PPA_ALPHA_SCALE) PF(BLEND1_RX_FIX_ALPHA, a.value); }
    else    { PF(BLEND0_RX_ALPHA_MOD, a.mode == PPA_ALPHA_FIX ? 1 : a.mode == PPA_ALPHA_SCALE ? 2 : 0); PF(BLEND0_RX_ALPHA_INV, a.mode == PPA_ALPHA_INVERT);
              if (a.mode == PPA_ALPHA_FIX || a.mode == PPA_ALPHA_SCALE) PF(BLEND0_RX_FIX_ALPHA, a.value); }
}

static void blend_start(int mode)                                           /* 0 blend, 1 fill */
{
    REG(PPA_CK_FG_LOW_REG) = 0xFFFFFF; REG(PPA_CK_FG_HIGH_REG) = 0;             /* colour keying off: low > high */
    REG(PPA_CK_BG_LOW_REG) = 0xFFFFFF; REG(PPA_CK_BG_HIGH_REG) = 0;
    REG(PPA_CK_DEFAULT_REG) = 0;
    PF(BLEND_BYPASS, 0); PF(BLEND_FIX_PIXEL_FILL_EN, mode);
    REG_SET(PPA_BLEND_TRANS_MODE_REG, BIT(PPA_BLEND_EN_S) | BIT(PPA_BLEND_TRANS_MODE_UPDATE_S));
}

int ppa_blend(const ppa_pic_t *bg, const ppa_pic_t *fg, const ppa_pic_t *out, ppa_a_t bga, ppa_a_t fga)
{
    if (!ok(bg) || !ok(fg) || !ok(out) || bg->w != fg->w || bg->h != fg->h || out->x + bg->w > out->pic_w || out->y + bg->h > out->pic_h || init()) return -1;
    REG_SET(PPA_BLEND_TRANS_MODE_REG, BIT(PPA_BLEND_RST_S)); REG_CLR(PPA_BLEND_TRANS_MODE_REG, BIT(PPA_BLEND_RST_S));
    desc_in(&dtx[1], bg); desc_in(&dtx[2], fg);
    desc_out(&drx, out, bg->w, bg->h);
    wb(bg); wb(fg); inv(out);
    dma2d_cfg_t tb = { .periph = DMA2D_PERIPH_PPA_BLEND, .periph_id = 2 }, tf = { .periph = DMA2D_PERIPH_PPA_BLEND, .periph_id = 3 };
    dma2d_cfg_t rx = { .periph = DMA2D_PERIPH_PPA_BLEND, .periph_id = 2 };
    if (dma2d_rx_start(1, &rx, &drx) || dma2d_tx_start(1, &tb, &dtx[1]) || dma2d_tx_start(2, &tf, &dtx[2])) return -1;
    PF(BLEND0_RX_CM, cm_id[bg->cm]); PF(BLEND1_RX_CM, cm_id[fg->cm]); PF(BLEND_TX_CM, cm_id[out->cm]);
    PF(BLEND0_RX_BYTE_SWAP_EN, 0); PF(BLEND1_RX_BYTE_SWAP_EN, 0); PF(BLEND0_RX_RGB_SWAP_EN, 0); PF(BLEND1_RX_RGB_SWAP_EN, 0);
    alpha_bl(false, bga); alpha_bl(true, fga);
    PF(BLEND_HB, bg->w); PF(BLEND_VB, bg->h);
    REG(PPA_INT_CLR_REG) = BIT(PPA_BLEND_EOF_INT_CLR_S) | BIT(PPA_BLEND_PARAM_CFG_ERR_INT_CLR_S);
    blend_start(0);
    int r = dma2d_rx_wait(1, 1000000, 0);
    dma2d_stop(2, -1); dma2d_stop(1, 1);
    inv(out);
    return r ? -1 : 0;
}

int ppa_fill(const ppa_pic_t *out, uint32_t color)
{
    if (!ok(out) || init()) return -1;
    REG_SET(PPA_BLEND_TRANS_MODE_REG, BIT(PPA_BLEND_RST_S)); REG_CLR(PPA_BLEND_TRANS_MODE_REG, BIT(PPA_BLEND_RST_S));
    desc_out(&drx, out, out->w, out->h);
    inv(out);
    dma2d_cfg_t rx = { .periph = DMA2D_PERIPH_PPA_BLEND, .periph_id = 2 };
    if (dma2d_rx_start(1, &rx, &drx)) return -1;
    REG(PPA_BLEND_FIX_PIXEL_REG) = color;
    PF(BLEND_TX_CM, cm_id[out->cm]);
    PF(BLEND_HB, out->w); PF(BLEND_VB, out->h);
    REG(PPA_INT_CLR_REG) = BIT(PPA_BLEND_EOF_INT_CLR_S);
    blend_start(1);
    int r = dma2d_rx_wait(1, 1000000, 0);
    dma2d_stop(-1, 1);
    inv(out);
    return r ? -1 : 0;
}
