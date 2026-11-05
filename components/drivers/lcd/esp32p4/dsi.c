/* P4 MIPI DSI (dsi.h). Flow of ESP-IDF v6.1 esp_lcd/dsi (esp_lcd_mipi_dsi_bus.c, esp_lcd_panel_dpi.c, esp_lcd_panel_io_dbi.c) and
 * esp_hal_lcd/mipi_dsi_hal.c + esp32p4/mipi_dsi_{ll,host_ll,phy_ll,brg_ll}.h. Registers: generated regs/dsi_host.h, dsi_bridge.h.
 * Bring-up: bus clock + reset, PHY config clock (PLL_F20M) and PLL reference clock (XTAL), PHY PLL set through the D-PHY test
 * interface (N / M / hs_freq range), wait for lock and stopped lanes, command mode. Video: DPI clock (PLL_F240M / div),
 * host timing scaled to the byte clock, bridge timing in pixel clocks, DW-GDMA streams the frame buffer into the bridge
 * (memory 0x50105000) as one block with the bridge as flow control by hardware handshake. NOT VERIFIED ON HARDWARE. */
#include "dsi.h"
#include "dw_gdma.h"
#include "ldo.h"
#include "clk.h"
#include "cpu.h"
#include "spin.h"
#include "soc.h"
#include "cache_p4.h"
#include "regs/dsi_host.h"
#include "regs/dsi_bridge.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"
#include <string.h>

#ifndef NDF_DSI_LDO_CHAN
#define NDF_DSI_LDO_CHAN 3
#endif
#ifndef NDF_DSI_LDO_MV
#define NDF_DSI_LDO_MV   2500
#endif
#define BRIDGE_MEM  0x50105000u
#define H(F, v)  FPUT(DSI_HOST_##F, v)
#define B(F, v)  FPUT(DSI_BRG_##F, v)
#define DT_DCS_SW0 0x05
#define DT_DCS_SW1 0x15
#define DT_DCS_LW  0x39
#define DT_DCS_RD0 0x06
#define DT_SET_MAX_RET 0x37

static struct { int lanes, mbps; uint32_t dpi_div; int dpi_khz_req; bool up, video; } d;
static dw_lli_t item __attribute__((aligned(64)));

static const struct { uint16_t lo, hi; uint8_t sel; } pll_range[] = {
    { 80, 89, 0x00 }, { 90, 99, 0x10 }, { 100, 109, 0x20 }, { 110, 129, 0x01 }, { 130, 139, 0x11 }, { 140, 149, 0x21 }, { 150, 169, 0x02 }, { 170, 179, 0x12 },
    { 180, 199, 0x22 }, { 200, 219, 0x03 }, { 220, 239, 0x13 }, { 240, 249, 0x23 }, { 250, 269, 0x04 }, { 270, 299, 0x14 }, { 300, 329, 0x05 }, { 330, 359, 0x15 },
    { 360, 399, 0x25 }, { 400, 449, 0x06 }, { 450, 499, 0x16 }, { 500, 549, 0x07 }, { 550, 599, 0x17 }, { 600, 649, 0x08 }, { 650, 699, 0x18 }, { 700, 749, 0x09 },
    { 750, 799, 0x19 }, { 800, 849, 0x29 }, { 850, 899, 0x39 }, { 900, 949, 0x0A }, { 950, 999, 0x1A }, { 1000, 1049, 0x2A }, { 1050, 1099, 0x3A }, { 1100, 1149, 0x0B },
    { 1150, 1199, 0x1B }, { 1200, 1249, 0x2B }, { 1250, 1299, 0x3B }, { 1300, 1349, 0x0C }, { 1350, 1399, 0x1C }, { 1400, 1449, 0x2C }, { 1450, 1500, 0x3C },
};

static void phy_write(uint8_t addr, uint8_t val)                          /* D-PHY test interface (mipi_dsi_hal_phy_write_register) */
{
    REG(DSI_HOST_PHY_TST_CTRL0_REG) = 0;
    REG(DSI_HOST_PHY_TST_CTRL1_REG) = BIT(16) | addr;
    REG(DSI_HOST_PHY_TST_CTRL0_REG) = BIT(1);
    REG(DSI_HOST_PHY_TST_CTRL0_REG) = 0;
    REG(DSI_HOST_PHY_TST_CTRL1_REG) = val;
    REG(DSI_HOST_PHY_TST_CTRL0_REG) = BIT(1);
    REG(DSI_HOST_PHY_TST_CTRL0_REG) = 0;
}

int dsi_init(const dsi_cfg_t *c)
{
    if (!c || c->lanes < 1 || c->lanes > 2 || c->lane_mbps < 80 || c->lane_mbps > 1500 || dwg_init()) return -1;
    if (NDF_DSI_LDO_CHAN) ldo_on(NDF_DSI_LDO_CHAN - 1, NDF_DSI_LDO_MV);
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_DSI_SYS_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_DSI_BRG); FCLR(HP_SYS_CLKRST_REG_RST_EN_DSI_BRG);
    FSET(HP_SYS_CLKRST_REG_REF_20M_CLK_EN);                                   /* the PLL_F20M reference gates */
    FPUT(HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_CLK_SRC_SEL, 0);                    /* configuration clock: PLL_F20M */
    FSET(HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_CFG_CLK_EN);
    FPUT(HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_SRC_SEL, 0);             /* PLL reference: XTAL (40 MHz) */
    FPUT(HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_DIV_NUM, 0);
    FSET(HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_EN);
    irq_unlock(t);
    d.lanes = c->lanes;
    REG_SET(DSI_BRG_CLK_EN_REG, BIT(DSI_BRG_CLK_EN_S));
    B(DSI_CFG_REF_CLK_EN, 1);
    H(N_LANES, c->lanes - 1);
    H(SHUTDOWNZ, 1);                                                          /* host on */
    H(PHY_SHUTDOWNZ, 1);                                                      /* PHY on, reset pulse, clock lane enable, force PLL */
    H(PHY_RSTZ, 0); H(PHY_RSTZ, 1);
    H(PHY_ENABLECLK, 1); H(PHY_FORCEPLL, 1);
    B(DSI_BRIG_RST, 1); B(DSI_BRIG_RST, 0);
    /* PHY PLL: reference 40 MHz, VCO = lane rate; N / M with M even (mipi_dsi_hal_configure_phy_pll) */
    int ref = 40, best = 0x7FFFFFFF, N = 1, M = 0;
    for (int n = 1 > ref / 40 ? 1 : ref / 40; n <= ref / 5; n++) {
        int m = c->lane_mbps * n / ref;
        if (m & 1) continue;
        int delta = c->lane_mbps * 100 - ref * 100 * m / n;
        if (delta < 0) delta = -delta;
        if (delta < best) { best = delta; M = m; N = n; }
    }
    uint8_t sel = 0;
    for (unsigned i = 0; i < sizeof pll_range / sizeof pll_range[0]; i++) if (c->lane_mbps >= pll_range[i].lo && c->lane_mbps <= pll_range[i].hi) { sel = pll_range[i].sel; break; }
    phy_write(0x44, sel << 1);
    phy_write(0x19, 0x30);
    phy_write(0x17, N - 1);
    phy_write(0x18, (M - 1) & 0x1F);
    phy_write(0x18, 0x80 | (((M - 1) >> 5) & 0x0F));
    d.mbps = ref * M / N;
    SPIN_US(!FGET(DSI_HOST_PHY_LOCK), 10000);
    if (!FGET(DSI_HOST_PHY_LOCK)) return -1;
    uint32_t mask = BIT(DSI_HOST_PHY_STOPSTATECLKLANE_S) | BIT(DSI_HOST_PHY_STOPSTATE0LANE_S) | (c->lanes > 1 ? BIT(DSI_HOST_PHY_STOPSTATE1LANE_S) : 0);
    SPIN_US((REG(DSI_HOST_PHY_STATUS_REG) & mask) != mask, 10000);
    if ((REG(DSI_HOST_PHY_STATUS_REG) & mask) != mask) return -1;
    FPUT(DSI_HOST_CMD_VIDEO_MODE, 1);                                        /* command mode */
    H(AUTO_CLKLANE_CTRL, !c->clock_lane_hs); H(PHY_TXREQUESTCLKHS, 1);
    H(PHY_LP2HS_TIME, 104); H(PHY_HS2LP_TIME, 50); H(PHY_CLKLP2HS_TIME, 128); H(PHY_CLKHS2LP_TIME, 46);
    H(CRC_RX_EN, 1); H(ECC_RX_EN, 1); H(EOTP_TX_EN, 1); H(EOTP_TX_LP_EN, 0);
    H(TO_CLK_DIVISION, (d.mbps / 8 + 0) / 1);                                 /* timeout clock 1 MHz */
    H(TX_ESC_CLK_DIVISION, (d.mbps / 8 + 10) / 20);                           /* escape clock <= 20 MHz */
    REG(DSI_HOST_TO_CNT_CFG_REG) = 0; REG(DSI_HOST_HS_RD_TO_CNT_REG) = 0; REG(DSI_HOST_LP_RD_TO_CNT_REG) = 0;
    REG(DSI_HOST_HS_WR_TO_CNT_REG) = 0; REG(DSI_HOST_LP_WR_TO_CNT_REG) = 0; REG(DSI_HOST_BTA_TO_CNT_REG) = 0;
    H(MAX_RD_TIME, 6000); H(PHY_STOP_WAIT_TIME, 0x3F);
    /* commands in low-power mode, with acknowledge */
    H(TEAR_FX_EN, 0); H(ACK_RQST_EN, 1);
    REG_SET(DSI_HOST_CMD_MODE_CFG_REG, BIT(DSI_HOST_GEN_SW_0P_TX_S) | BIT(DSI_HOST_GEN_SW_1P_TX_S) | BIT(DSI_HOST_GEN_SW_2P_TX_S) | BIT(DSI_HOST_GEN_LW_TX_S) |
                                       BIT(DSI_HOST_GEN_SR_0P_TX_S) | BIT(DSI_HOST_GEN_SR_1P_TX_S) | BIT(DSI_HOST_GEN_SR_2P_TX_S) | BIT(DSI_HOST_DCS_SW_0P_TX_S) |
                                       BIT(DSI_HOST_DCS_SW_1P_TX_S) | BIT(DSI_HOST_DCS_LW_TX_S) | BIT(DSI_HOST_DCS_SR_0P_TX_S) | BIT(DSI_HOST_MAX_RD_PKT_SIZE_S));
    d.up = true;
    return 0;
}

static void pkt_hdr(int vc, int dt, uint8_t msb, uint8_t lsb)
{
    SPIN_US(FGET(DSI_HOST_GEN_CMD_FULL), 100000);
    REG(DSI_HOST_GEN_HDR_REG) = (uint32_t)msb << 16 | (uint32_t)lsb << 8 | (vc << 6 | dt);
}
static void pld(uint32_t w)
{
    SPIN_US(FGET(DSI_HOST_GEN_PLD_W_FULL), 100000);
    REG(DSI_HOST_GEN_PLD_DATA_REG) = w;
}

int dsi_dcs_write(int vc, uint8_t cmd, const void *param, size_t n)
{
    if (!d.up || vc < 0 || vc > 3 || n > 0xFFF0) return -1;
    const uint8_t *p = param;
    size_t total = 1 + n;
    if (total > 2) {                                                          /* long write: payload words, then the header */
        uint32_t w = cmd; size_t used = 0;
        for (; used < n && used < 3; used++) w |= (uint32_t)p[used] << (8 * (used + 1));
        pld(w); p += used;
        size_t left = n - used;
        for (; left >= 4; left -= 4, p += 4) { uint32_t x; memcpy(&x, p, 4); pld(x); }
        if (left) { uint32_t x = 0; memcpy(&x, p, left); pld(x); }
        pkt_hdr(vc, DT_DCS_LW, total >> 8, total & 0xFF);
    } else if (total == 2) pkt_hdr(vc, DT_DCS_SW1, p[0], cmd);
    else pkt_hdr(vc, DT_DCS_SW0, 0, cmd);
    SPIN_US(!FGET(DSI_HOST_GEN_CMD_EMPTY) || !FGET(DSI_HOST_GEN_PLD_W_EMPTY), 100000);
    return 0;
}

int dsi_dcs_read(int vc, uint8_t cmd, void *buf, size_t n)
{
    if (!d.up || vc < 0 || vc > 3 || !buf || !n || n > 0xFFFF) return -1;
    FPUT(DSI_HOST_CMD_VIDEO_MODE, 1);
    pkt_hdr(vc, DT_SET_MAX_RET, n >> 8, n & 0xFF);
    H(BTA_EN, 1); H(GEN_VCID_RX, vc);
    pkt_hdr(vc, DT_DCS_RD0, 0, cmd);
    SPIN_US(FGET(DSI_HOST_GEN_RD_CMD_BUSY), 100000);
    SPIN_US(FGET(DSI_HOST_GEN_PLD_R_EMPTY), 100000);
    uint8_t *o = buf; size_t k = 0;
    while (!FGET(DSI_HOST_GEN_PLD_R_EMPTY)) {
        uint32_t w = REG(DSI_HOST_GEN_PLD_DATA_REG);
        for (int i = 0; i < 4 && k < n; i++) o[k++] = w >> (8 * i);
    }
    return (int)k;
}

int dsi_video_start(const dsi_video_t *v, void *fb)
{
    if (!d.up || !v || !fb || ((uintptr_t)fb & 63) || (v->bpp != 16 && v->bpp != 24) || v->dpi_khz < 1000 || v->vc < 0 || v->vc > 3) return -1;
    uint32_t src_khz = 240000, div = (src_khz + v->dpi_khz / 2) / v->dpi_khz;                      /* DPI clock source PLL_F240M */
    if (div < 1 || div > 256) return -1;
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_REF_240M_CLK_EN);
    FPUT(HP_SYS_CLKRST_REG_MIPI_DSI_DPICLK_SRC_SEL, 1);
    FPUT(HP_SYS_CLKRST_REG_MIPI_DSI_DPICLK_DIV_NUM, div - 1);
    FSET(HP_SYS_CLKRST_REG_MIPI_DSI_DPICLK_EN);
    irq_unlock(t);
    float real_khz = (float)src_khz / (float)div, ratio = (float)d.mbps * 1000.0f / (float)v->dpi_khz / 8.0f;       /* byte clock cycles per pixel clock */
    (void)real_khz;
    H(DPI_VCID, v->vc);
    H(DPI_COLOR_CODING, v->bpp == 16 ? 0 : 5); H(LOOSELY18_EN, 0);
    REG(DSI_HOST_DPI_CFG_POL_REG) = 0;
    H(INVACT_LPCMD_TIME, 0); H(OUTVACT_LPCMD_TIME, 0);
    REG_SET(DSI_HOST_VID_MODE_CFG_REG, BIT(DSI_HOST_LP_VSA_EN_S) | BIT(DSI_HOST_LP_VBP_EN_S) | BIT(DSI_HOST_LP_VFP_EN_S) | BIT(DSI_HOST_LP_VACT_EN_S) |
                                       BIT(DSI_HOST_LP_HBP_EN_S) | BIT(DSI_HOST_LP_HFP_EN_S) | BIT(DSI_HOST_FRAME_BTA_ACK_EN_S) | BIT(DSI_HOST_LP_CMD_EN_S));
    FPUT(DSI_HOST_VID_MODE_TYPE, 2);                                          /* burst with sync pulses */
    H(VID_PKT_SIZE, v->h_size); H(VID_NUM_CHUNKS, 0); H(VID_NULL_SIZE, 0);
    uint32_t hs = (uint32_t)(v->hsync * ratio + 0.5f), hb = (uint32_t)(v->hbp * ratio + 0.5f), htot = (uint32_t)((v->hsync + v->hbp + v->h_size + v->hfp) * ratio + 0.5f);
    H(VID_HSA_TIME, hs); H(VID_HBP_TIME, hb); H(VID_HLINE_TIME, htot);
    H(VSA_LINES, v->vsync); H(VBP_LINES, v->vbp); H(VFP_LINES, v->vfp); H(V_ACTIVE_LINES, v->v_size);
    B(HDISP, v->h_size); B(HTOTAL, v->hsync + v->hbp + v->h_size + v->hfp); B(HSYNC, v->hsync); B(HBANK, v->hbp);
    B(VDISP, v->v_size); B(VTOTAL, v->vsync + v->vbp + v->v_size + v->vfp); B(VSYNC, v->vsync); B(VBANK, v->vbp);
    uint32_t bits = (uint32_t)v->h_size * v->v_size * v->bpp;
    REG(DSI_BRG_RAW_NUM_CFG_REG) = ((bits + 63) / 64) | ((bits % 64) ? BIT(DSI_BRG_UNALIGN_64BIT_EN_S) : 0) | BIT(DSI_BRG_RAW_NUM_TOTAL_SET_S);
    B(FIFO_UNDERRUN_DISCARD_VCNT, v->h_size);
    B(RAW_TYPE, v->bpp == 16 ? 2 : 0); B(DATA_IN_TYPE, 0); B(DPI_TYPE, v->bpp == 16 ? 2 : 0); B(DPI_CONFIG, 0);
    B(DSI_DMA_FLOW_CONTROLLER, 0); B(DMA_FLOW_MULTIBLK_NUM, 1); B(DMA_MULTIBLK_EN, 0);
    B(DMA_BURST_LEN, 256); B(DSI_RAW_BUF_ALMOST_EMPTY_THRD, 1024 - 256);
    B(DSI_EN, 1);
    REG_SET(DSI_BRG_DPI_CONFIG_UPDATE_REG, BIT(DSI_BRG_DPI_CONFIG_UPDATE_S));
    /* frame buffer -> bridge FIFO: 64-bit transfers, source bursts incrementing, destination fixed (the FIFO), one block */
    size_t fbs = (size_t)v->h_size * v->v_size * v->bpp / 8;
    cache_dma_writeback(fb, fbs);
    if (dwg_open(0, DWG_MEM, DWG_DSI, 1)) return -1;
    dwg_block_t blk = { (uint32_t)(uintptr_t)fb, BRIDGE_MEM, (uint32_t)(fbs * 8 / 64), DWG_WIDTH_64, DWG_ITEMS_512, DWG_ITEMS_256, 16, false, true, true, false };
    dwg_lli_fill(&item, &blk, 0);
    if (dwg_start(0, &item)) return -1;
    FPUT(DSI_HOST_CMD_VIDEO_MODE, 0);                                         /* video mode */
    B(DPI_EN, 1);
    REG_SET(DSI_BRG_DPI_CONFIG_UPDATE_REG, BIT(DSI_BRG_DPI_CONFIG_UPDATE_S));
    REG_SET(DSI_BRG_INT_ENA_REG, BIT(DSI_BRG_UNDERRUN_INT_ENA_S));
    d.video = true;
    return 0;
}

void dsi_video_stop(void)
{
    if (!d.video) return;
    B(DPI_EN, 0);
    REG_SET(DSI_BRG_DPI_CONFIG_UPDATE_REG, BIT(DSI_BRG_DPI_CONFIG_UPDATE_S));
    dwg_stop(0);
    FPUT(DSI_HOST_CMD_VIDEO_MODE, 1);
    d.video = false;
}

uint32_t dsi_underruns(void)
{
    return (REG(DSI_BRG_INT_RAW_REG) & BIT(DSI_BRG_UNDERRUN_INT_RAW_S)) ? 1 : 0;      /* sticky flag; the application clears with dsi_video_stop() */
}
