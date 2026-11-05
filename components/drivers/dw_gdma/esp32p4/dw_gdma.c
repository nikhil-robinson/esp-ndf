/* P4 DW-GDMA (dw_gdma.h). Registers: generated regs/dw_gdma.h (channel 1 names, +0x100 per channel); flow of ESP-IDF v6.1
 * esp_driver_dma/src/dw_gdma.c and esp_hal_dma/esp32p4/dw_gdma_ll.h: master port 1 reaches L2MEM / PSRAM / flash, port 0 the
 * DSI / CSI bridge memories; block size register is items - 1; the link-list head address is in units of 64 bytes. */
#include "dw_gdma.h"
#include "clk.h"
#include "cpu.h"
#include "spin.h"
#include "soc.h"
#include "cache_p4.h"
#include <string.h>
#include "regs/dw_gdma.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"

#define CHF(c, F, v)   REG_FIELD(DMAC_CH1_##F##_R + 0x100 * (c), DMAC_CH1_##F##_S, DMAC_CH1_##F##_W, v)
#define CHR(c, r)      (DMAC_CH1_##r##_REG + 0x100 * (c))
#define MASTER_MEM     1
#define MASTER_BRIDGE  0
#define BRIDGE_DSI     0x50105000u
#define BRIDGE_CSI     0x50104000u
enum { MULTBLK_CONTIG = 0, MULTBLK_RELOAD = 1, MULTBLK_SHADOW = 2, MULTBLK_LIST = 3 };
enum { FC_M2M = 0, FC_M2P = 1, FC_P2M = 2 };

static bool up;

int dwg_init(void)
{
    if (up) return 0;
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_GDMA_CPU_CLK_EN); FSET(HP_SYS_CLKRST_REG_GDMA_SYS_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_GDMA); FCLR(HP_SYS_CLKRST_REG_RST_EN_GDMA);
    irq_unlock(t);
    REG_SET(DMAC_RESET0_REG, BIT(DMAC_DMAC_RST_S));
    SPIN_WHILE(REG(DMAC_RESET0_REG) & BIT(DMAC_DMAC_RST_S), 1000);
    REG_SET(DMAC_CFG0_REG, BIT(DMAC_DMAC_EN_S));
    up = true;
    return 0;
}

static uint32_t port_of(uint32_t addr) { return (addr == BRIDGE_DSI || addr == BRIDGE_CSI) ? MASTER_BRIDGE : MASTER_MEM; }

static void ctl_words(const dwg_block_t *b, uint32_t *lo, uint32_t *hi)
{
    *lo = FV(DMAC_CH1_SMS, port_of(b->src)) | FV(DMAC_CH1_DMS, port_of(b->dst)) | FV(DMAC_CH1_SINC, b->src_fixed) | FV(DMAC_CH1_DINC, b->dst_fixed) |
                FV(DMAC_CH1_SRC_TR_WIDTH, b->width) | FV(DMAC_CH1_DST_TR_WIDTH, b->width) | FV(DMAC_CH1_SRC_MSIZE, b->src_items) | FV(DMAC_CH1_DST_MSIZE, b->dst_items);
    *hi = (b->burst_len ? FV(DMAC_CH1_ARLEN_EN, 1) | FV(DMAC_CH1_ARLEN, b->burst_len) | FV(DMAC_CH1_AWLEN_EN, 1) | FV(DMAC_CH1_AWLEN, b->burst_len) : 0) |
                FV(DMAC_CH1_IOC_BLKTFR, b->irq) | FV(DMAC_CH1_SHADOWREG_OR_LLI_LAST, b->last) | FV(DMAC_CH1_SHADOWREG_OR_LLI_VALID, 1);
}

void dwg_lli_fill(dw_lli_t *l, const dwg_block_t *b, dw_lli_t *next)
{
    memset(l, 0, sizeof *l);
    l->sar_lo = b->src; l->dar_lo = b->dst;
    l->block_ts = b->items - 1;
    ctl_words(b, &l->ctl_lo, &l->ctl_hi);
    l->llp_lo = next ? (uint32_t)(uintptr_t)next >> 6 << DMAC_CH1_LOC0_S | FV(DMAC_CH1_LMS, MASTER_MEM) : 0;
    cache_dma_writeback(l, sizeof *l);
}

int dwg_open(int ch, dwg_role_t src, dwg_role_t dst, int prio)
{
    if ((unsigned)ch > 3 || dwg_init()) return -1;
    int fc = src == DWG_MEM ? (dst == DWG_MEM ? FC_M2M : FC_M2P) : FC_P2M;
    CHF(ch, TT_FC, fc);
    CHF(ch, SRC_MULTBLK_TYPE, MULTBLK_LIST); CHF(ch, DST_MULTBLK_TYPE, MULTBLK_LIST);
    CHF(ch, HS_SEL_SRC, 0); CHF(ch, HS_SEL_DST, 0);                          /* hardware handshake */
    if (src != DWG_MEM) CHF(ch, SRC_PER, src - 1);
    if (dst != DWG_MEM) CHF(ch, DST_PER, dst - 1);
    CHF(ch, CH_PRIOR, prio);
    CHF(ch, SRC_OSR_LMT, src == DWG_MEM ? 4 : 1);                              /* outstanding requests - 1 (5 / 2 as ESP-IDF's DSI) */
    CHF(ch, DST_OSR_LMT, dst == DWG_MEM ? 4 : 1);
    REG(CHR(ch, INTSTATUS_ENABLE0)) = 0xFFFFFFFFu;                             /* status bits on, no interrupt signal: polled */
    REG(CHR(ch, INTSIGNAL_ENABLE0)) = 0;
    return 0;
}

int dwg_start(int ch, dw_lli_t *first)
{
    if ((unsigned)ch > 3 || !first || !up) return -1;
    REG(CHR(ch, INTCLEAR0)) = 0xFFFFFFFFu;
    REG(CHR(ch, LLP0)) = ((uint32_t)(uintptr_t)first >> 6 << DMAC_CH1_LOC0_S) | FV(DMAC_CH1_LMS, MASTER_MEM);
    REG(CHR(ch, LLP1)) = 0;
    REG(DMAC_CHEN0_REG) = 0x101u << ch;                                        /* enable (+ write-enable bit) */
    return 0;
}

int dwg_set_flow(int ch, int tt_fc, uint32_t src_status_addr)
{
    if ((unsigned)ch > 3 || !up) return -1;
    CHF(ch, TT_FC, tt_fc);                                                    /* 0..7: M2M/M2P/P2M by the DMAC, then P2M_SRC = 4, P2P_SRC = 5, M2P_DST = 6, P2P_DST = 7 */
    CHF(ch, SRC_MULTBLK_TYPE, MULTBLK_CONTIG); CHF(ch, DST_MULTBLK_TYPE, MULTBLK_CONTIG);
    REG(CHR(ch, SSTATAR0)) = src_status_addr;
    return 0;
}

int dwg_run(int ch, const dwg_block_t *b)
{
    if ((unsigned)ch > 3 || !b || !up) return -1;
    uint32_t lo, hi;
    ctl_words(b, &lo, &hi);
    hi &= ~(FMASK(DMAC_CH1_SHADOWREG_OR_LLI_VALID) | FMASK(DMAC_CH1_SHADOWREG_OR_LLI_LAST));    /* contiguous: no list markers */
    REG(CHR(ch, INTCLEAR0)) = 0xFFFFFFFFu;
    REG(CHR(ch, SAR0)) = b->src; REG(CHR(ch, DAR0)) = b->dst;
    REG(CHR(ch, BLOCK_TS0)) = b->items - 1;
    REG(CHR(ch, CTL0)) = lo; REG(CHR(ch, CTL1)) = hi;
    REG(DMAC_CHEN0_REG) = 0x101u << ch;
    return 0;
}

uint32_t dwg_status(int ch) { return (unsigned)ch < 4 ? REG(CHR(ch, INTSTATUS0)) : 0; }

int dwg_wait(int ch, uint32_t timeout_us)
{
    if ((unsigned)ch > 3) return -1;
    const uint32_t done = BIT(DMAC_CH1_DMA_TFR_DONE_INTSTAT_S), err = 0x023F7FE0u;   /* bits 5..14 decode / slave / LLI errors, 16..21 and 25 slave-interface errors */
    SPIN_WHILE(!(REG(CHR(ch, INTSTATUS0)) & (done | err)), timeout_us);
    uint32_t s = REG(CHR(ch, INTSTATUS0));
    return (s & done) && !(s & err) ? 0 : -1;
}

void dwg_stop(int ch)
{
    if ((unsigned)ch > 3) return;
    REG(DMAC_CHEN0_REG) = 0x100u << ch;                                        /* write-enable only: clears the enable bit */
    SPIN_WHILE(REG(DMAC_CHEN0_REG) & BIT(ch), 1000);
    REG(CHR(ch, INTCLEAR0)) = 0xFFFFFFFFu;
}

int dwg_m2m(void *dst, const void *src, size_t n)
{
    static dw_lli_t item;
    if (!n || (n & 7) || n > (4u << 20) * 8 || dwg_open(3, DWG_MEM, DWG_MEM, 0)) return -1;
    cache_dma_writeback(src, n);
    cache_dma_wbinv(dst, n);                                    /* dst's dirty lines must not be written back over the DMA's data */
    dwg_block_t b = { (uint32_t)(uintptr_t)src, (uint32_t)(uintptr_t)dst, (uint32_t)(n / 8), DWG_WIDTH_64, DWG_ITEMS_16, DWG_ITEMS_16, 0, false, false, true, true };
    dwg_lli_fill(&item, &b, 0);
    if (dwg_start(3, &item)) return -1;
    int r = dwg_wait(3, 1000000);
    dwg_stop(3);
    cache_dma_wbinv(dst, n);
    return r;
}
