/* P4 2D-DMA (dma2d.h). Sequence of ESP-IDF v6.1 esp_driver_dma/src/dma2d.c and esp_hal_dma/esp32p4/dma2d_ll.h (channel set-up:
 * stop, abort + reset, connect to the peripheral, owner check off, descriptor-end EOF mode, 128-byte bursts, page bound
 * wrap), registers from the generated regs/dma2d.h (channel 0 names; channel n is +0x100 n, RX block at +0x500). */
#include "dma2d.h"
#include "clk.h"
#include "soc.h"
#include "cpu.h"
#include "spin.h"
#include "cache_p4.h"
#include "regs/dma2d.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"

#define OUTF(n, F, v) REG_FIELD(DMA2D_OUT_##F##_CH0_R + 0x100 * (n), DMA2D_OUT_##F##_CH0_S, DMA2D_OUT_##F##_CH0_W, v)
#define INF(n, F, v)  REG_FIELD(DMA2D_IN_##F##_CH0_R + 0x100 * (n), DMA2D_IN_##F##_CH0_S, DMA2D_IN_##F##_CH0_W, v)
#define OUTG(n, F)    ((REG(DMA2D_OUT_##F##_CH0_R + 0x100 * (n)) >> DMA2D_OUT_##F##_CH0_S) & ((1u << DMA2D_OUT_##F##_CH0_W) - 1))
#define ING(n, F)     ((REG(DMA2D_IN_##F##_CH0_R + 0x100 * (n)) >> DMA2D_IN_##F##_CH0_S) & ((1u << DMA2D_IN_##F##_CH0_W) - 1))
#define TX(n, r)  (DMA2D_OUT_##r##_CH0_REG + 0x100 * (n))
#define RX(n, r)  (DMA2D_IN_##r##_CH0_REG + 0x100 * (n))
#define EV_DONE   BIT(0)
#define EV_EOF    BIT(1)                     /* TX: suc_eof descriptor sent; RX: suc_eof descriptor written */
#define EV_ERR_TX BIT(2)                     /* TX: descriptor error */
#define EV_ERR_RX (BIT(2) | BIT(3))          /* RX: err_eof, descriptor error */
#define M2M_ID    4                          /* any id 4..7 (TX) / 3..7 (RX) outside the peripherals' ids; both ends use the same */

static bool up;

int dma2d_init(void)
{
    if (up) return 0;
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_DMA2D_SYS_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_DMA2D); FCLR(HP_SYS_CLKRST_REG_RST_EN_DMA2D);
    irq_unlock(t);
    REG_SET(DMA2D_RST_CONF_REG, BIT(DMA2D_CLK_EN_S));                             /* hw enable, flush the AXI master FIFOs */
    REG_SET(DMA2D_RST_CONF_REG, BIT(DMA2D_AXIM_RD_RST_S) | BIT(DMA2D_AXIM_WR_RST_S));
    REG_CLR(DMA2D_RST_CONF_REG, BIT(DMA2D_AXIM_RD_RST_S) | BIT(DMA2D_AXIM_WR_RST_S));
    REG(DMA2D_OUT_ARB_CONFIG_REG) = 0;                                            /* no arbiter weights / timeout */
    REG(DMA2D_IN_ARB_CONFIG_REG) = 0;
    up = true;
    return 0;
}

void dma2d_desc_1d(dma2d_desc_t *d, void *buf, uint32_t size, uint32_t len, bool eof, dma2d_desc_t *next)
{
    d->w0 = (size & 0x3FFF) | (len & 0x3FFF) << 14 | (eof ? BIT(30) : 0) | BIT(31);       /* dma2d_en = 0: byte sizes, low 14 bits here */
    d->w1 = (size >> 14 & 0x3FFF) | (len >> 14 & 0x3FFF) << 14;
    d->w2 = 0;
    d->buf = buf; d->next = next;
}

void dma2d_desc_2d(dma2d_desc_t *d, void *buf, int ha, int va, int hb, int vb, int x, int y, int pbyte, bool multiple, bool eof, dma2d_desc_t *next)
{
    d->w0 = (vb & 0x3FFF) | (hb & 0x3FFF) << 14 | BIT(29) | (eof ? BIT(30) : 0) | BIT(31);
    d->w1 = (va & 0x3FFF) | (ha & 0x3FFF) << 14 | (uint32_t)pbyte << 28;
    d->w2 = (y & 0x3FFF) | (x & 0x3FFF) << 14 | (multiple ? BIT(28) : 0);
    d->buf = buf; d->next = next;
}

static uint32_t mb_sel(dma2d_mb_t m) { return m == DMA2D_MB_NONE ? 3 : m - 1; }

static void flush_list(dma2d_desc_t *d, bool tx_data)
{
    for (; d; d = d->next) {
        uint32_t pix = 0;
        cache_dma_writeback(d, sizeof *d);
        if (tx_data && !(d->w0 & BIT(29))) pix = (d->w1 >> 14 & 0x3FFF) << 14 | (d->w0 >> 14 & 0x3FFF);      /* 1-D: length */
        else if (tx_data) pix = (d->w1 >> 14 & 0x3FFF) * (d->w1 & 0x3FFF) * 4;                              /* 2-D: whole picture, upper bound */
        if (pix) cache_dma_writeback(d->buf, pix);
        if (d->w0 & BIT(30)) break;
    }
}

int dma2d_tx_start(int ch, const dma2d_cfg_t *c, dma2d_desc_t *first)
{
    if (!up || (unsigned)ch >= DMA2D_TX_CHANNELS || !c || !first) return -1;
    int id = c->periph == DMA2D_PERIPH_M2M ? M2M_ID : c->periph_id;
    REG_CLR(TX(ch, LINK_CONF), BIT(DMA2D_OUTLINK_START_CH0_S));
    REG_SET(TX(ch, LINK_CONF), BIT(DMA2D_OUTLINK_STOP_CH0_S));
    OUTF(ch, CMD_DISABLE, 1);                                                  /* abort, wait until the channel can be reset, reset */
    SPIN_US(!OUTG(ch, RESET_AVAIL), 1000);
    OUTF(ch, RST, 1); OUTF(ch, RST, 0); OUTF(ch, CMD_DISABLE, 0);
    OUTF(ch, PERI_SEL, id);
    OUTF(ch, CHECK_OWNER, 0); OUTF(ch, AUTO_WRBACK, 0); OUTF(ch, EOF_MODE, 1);
    OUTF(ch, MEM_BURST_LENGTH, 4); OUTF(ch, PAGE_BOUND_EN, 1);                 /* 128-byte bursts */
    OUTF(ch, MACRO_BLOCK_SIZE, mb_sel(c->mb));
    OUTF(ch, REORDER_EN, c->reorder && ch == 0);
    OUTF(ch, DSCR_PORT_EN, c->dscr_port);
    if (c->dscr_port) { OUTF(ch, DSCR_PORT_BLK_H, c->dscr_port_h); OUTF(ch, DSCR_PORT_BLK_V, c->dscr_port_v); }
    REG(TX(ch, INT_CLR)) = 0x1FFF;
    flush_list(first, true);
    REG(TX(ch, LINK_ADDR)) = (uint32_t)(uintptr_t)first;
    REG_CLR(TX(ch, LINK_CONF), BIT(DMA2D_OUTLINK_STOP_CH0_S));
    REG_SET(TX(ch, LINK_CONF), BIT(DMA2D_OUTLINK_START_CH0_S));
    return 0;
}

int dma2d_rx_start(int ch, const dma2d_cfg_t *c, dma2d_desc_t *first)
{
    if (!up || (unsigned)ch >= DMA2D_RX_CHANNELS || !c || !first) return -1;
    int id = c->periph == DMA2D_PERIPH_M2M ? M2M_ID : c->periph_id;
    REG_CLR(RX(ch, LINK_CONF), BIT(DMA2D_INLINK_START_CH0_S));
    REG_SET(RX(ch, LINK_CONF), BIT(DMA2D_INLINK_STOP_CH0_S));
    INF(ch, CMD_DISABLE, 1);
    SPIN_US(!ING(ch, RESET_AVAIL), 1000);
    INF(ch, RST, 1); INF(ch, RST, 0); INF(ch, CMD_DISABLE, 0);
    INF(ch, PERI_SEL, id);
    INF(ch, MEM_TRANS_EN, c->periph == DMA2D_PERIPH_M2M);
    INF(ch, CHECK_OWNER, 0);
    REG_CLR(RX(ch, LINK_CONF), BIT(DMA2D_INLINK_AUTO_RET_CH0_S));              /* auto-return owner to the CPU */
    INF(ch, MEM_BURST_LENGTH, 4); INF(ch, PAGE_BOUND_EN, 1);
    INF(ch, MACRO_BLOCK_SIZE, mb_sel(c->mb));
    INF(ch, REORDER_EN, c->reorder && ch == 0);
    INF(ch, DSCR_PORT_EN, c->dscr_port);
    REG(RX(ch, INT_CLR)) = 0x3FFF;
    for (dma2d_desc_t *d = first; d; d = d->next) { cache_dma_wbinv(d, sizeof *d); if (d->w0 & BIT(30)) break; }
    REG(RX(ch, LINK_ADDR)) = (uint32_t)(uintptr_t)first;
    REG_CLR(RX(ch, LINK_CONF), BIT(DMA2D_INLINK_STOP_CH0_S));
    REG_SET(RX(ch, LINK_CONF), BIT(DMA2D_INLINK_START_CH0_S));
    return 0;
}

int dma2d_tx_wait(int ch, uint32_t timeout_us)
{
    if ((unsigned)ch >= DMA2D_TX_CHANNELS) return -1;
    SPIN_US(!(REG(TX(ch, INT_RAW)) & (EV_EOF | EV_ERR_TX)), timeout_us);
    return (REG(TX(ch, INT_RAW)) & EV_EOF) && !(REG(TX(ch, INT_RAW)) & EV_ERR_TX) ? 0 : -1;
}

int dma2d_rx_wait(int ch, uint32_t timeout_us, uint32_t *received)
{
    if ((unsigned)ch >= DMA2D_RX_CHANNELS) return -1;
    SPIN_US(!(REG(RX(ch, INT_RAW)) & (EV_EOF | EV_ERR_RX)), timeout_us);
    uint32_t raw = REG(RX(ch, INT_RAW));
    if (received) {                                                           /* hb / ha written back into the end descriptor */
        dma2d_desc_t *e = (dma2d_desc_t *)(uintptr_t)REG(RX(ch, SUC_EOF_DES_ADDR));
        *received = e ? (cache_dma_wbinv(e, sizeof *e), (e->w1 >> 14 & 0x3FFF) << 14 | (e->w0 >> 14 & 0x3FFF)) : 0;
    }
    return (raw & EV_EOF) && !(raw & EV_ERR_RX) ? 0 : -1;
}

uint32_t dma2d_rx_status(int ch) { return (unsigned)ch < DMA2D_RX_CHANNELS ? REG(RX(ch, INT_RAW)) : 0; }

void dma2d_stop(int tx_ch, int rx_ch)
{
    if ((unsigned)tx_ch < DMA2D_TX_CHANNELS) {
        REG_SET(TX(tx_ch, LINK_CONF), BIT(DMA2D_OUTLINK_STOP_CH0_S));
        OUTF(tx_ch, CMD_DISABLE, 1); SPIN_US(!OUTG(tx_ch, RESET_AVAIL), 1000);
        OUTF(tx_ch, RST, 1); OUTF(tx_ch, RST, 0); OUTF(tx_ch, CMD_DISABLE, 0);
        OUTF(tx_ch, PERI_SEL, 7);                                              /* 7: not connected */
    }
    if ((unsigned)rx_ch < DMA2D_RX_CHANNELS) {
        REG_SET(RX(rx_ch, LINK_CONF), BIT(DMA2D_INLINK_STOP_CH0_S));
        INF(rx_ch, CMD_DISABLE, 1); SPIN_US(!ING(rx_ch, RESET_AVAIL), 1000);
        INF(rx_ch, RST, 1); INF(rx_ch, RST, 0); INF(rx_ch, CMD_DISABLE, 0);
        INF(rx_ch, PERI_SEL, 7);
    }
}

int dma2d_m2m(void *dst, const void *src, size_t n)
{
    static dma2d_desc_t td __attribute__((aligned(64))), rd __attribute__((aligned(64)));
    if (!n || n > 0x0FFFFFFF || dma2d_init()) return -1;
    dma2d_cfg_t c = { .periph = DMA2D_PERIPH_M2M, .periph_id = -1 };
    dma2d_desc_1d(&td, (void *)src, n, n, true, 0);
    dma2d_desc_1d(&rd, dst, n, 0, true, 0);                                    /* receive: size n, the length is written back */
    if (dma2d_rx_start(2, &c, &rd) || dma2d_tx_start(3, &c, &td)) return -1;
    int r = dma2d_tx_wait(3, 1000000) | dma2d_rx_wait(2, 1000000, 0);
    cache_dma_wbinv(dst, n);
    dma2d_stop(3, 2);
    return r;
}
