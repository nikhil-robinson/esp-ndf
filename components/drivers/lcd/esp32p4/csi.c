/* P4 MIPI CSI-2 receiver (csi.h). Flow of ESP-IDF v6.1 esp_driver_cam/csi/src/esp_cam_ctlr_csi.c, esp_hal_cam/mipi_csi_hal.c,
 * esp32p4/mipi_csi_{ll,host_ll,brg_ll,phy_ll}.h and esp_hw_support/mipi_csi_share_hw_ctrl.c. Registers: generated regs/csi_host.h,
 * csi_bridge.h (the bridge names carry the TRM's "CSI_BRIG_" spelling). NOT VERIFIED ON HARDWARE. */
#include "csi.h"
#include "dw_gdma.h"
#include "ldo.h"
#include "clk.h"
#include "cpu.h"
#include "spin.h"
#include "soc.h"
#include "cache_p4.h"
#include "regs/csi_host.h"
#include "regs/csi_bridge.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"

#ifndef NDF_DSI_LDO_CHAN
#define NDF_DSI_LDO_CHAN 3
#endif
#ifndef NDF_DSI_LDO_MV
#define NDF_DSI_LDO_MV   2500
#endif
#define CSI_BRIDGE_MEM 0x50104000u
#define BR(F, v) FPUT(CSI_BRIG_##F, v)
#define HO(F, v) FPUT(CSI_HOST_##F, v)

static const struct { uint16_t lo, hi; uint8_t sel; } rng[] = {
    { 90, 99, 0x00 }, { 100, 109, 0x10 }, { 110, 129, 0x20 }, { 130, 139, 0x01 }, { 140, 149, 0x11 }, { 150, 169, 0x21 }, { 170, 179, 0x02 }, { 180, 199, 0x12 },
    { 200, 219, 0x22 }, { 220, 239, 0x03 }, { 240, 249, 0x13 }, { 250, 269, 0x23 }, { 270, 299, 0x04 }, { 300, 329, 0x14 }, { 330, 359, 0x05 }, { 360, 399, 0x15 },
    { 400, 449, 0x25 }, { 450, 499, 0x06 }, { 500, 549, 0x16 }, { 550, 599, 0x07 }, { 600, 649, 0x17 }, { 650, 699, 0x08 }, { 700, 749, 0x18 }, { 750, 799, 0x09 },
    { 800, 849, 0x19 }, { 850, 899, 0x29 }, { 900, 949, 0x39 }, { 950, 999, 0x0A }, { 1000, 1049, 0x1A }, { 1050, 1099, 0x2A }, { 1100, 1149, 0x3A }, { 1150, 1199, 0x0B },
    { 1200, 1249, 0x1B }, { 1250, 1299, 0x2B }, { 1300, 1349, 0x3B }, { 1350, 1399, 0x0C }, { 1400, 1449, 0x1C }, { 1450, 1500, 0x2C },
};
static struct { int h, v, bpp; bool up; } s;

static void phy_write(uint8_t addr, uint8_t val)
{
    REG(CSI_HOST_PHY_TEST_CTRL0_REG) = 0;
    REG(CSI_HOST_PHY_TEST_CTRL1_REG) = BIT(16) | addr;
    REG(CSI_HOST_PHY_TEST_CTRL0_REG) = BIT(1);
    REG(CSI_HOST_PHY_TEST_CTRL0_REG) = 0;
    REG(CSI_HOST_PHY_TEST_CTRL1_REG) = val;
    REG(CSI_HOST_PHY_TEST_CTRL0_REG) = BIT(1);
    REG(CSI_HOST_PHY_TEST_CTRL0_REG) = 0;
}

int csi_init(const csi_cfg_t *c)
{
    if (!c || c->lanes < 1 || c->lanes > 2 || c->lane_mbps < 90 || c->lane_mbps > 1500 || c->h_res < 1 || c->v_res < 1 || c->bits_per_pixel < 8 ||
        c->bits_per_pixel > 32 || ((uint64_t)c->h_res * c->v_res * c->bits_per_pixel) & 63 || dwg_init()) return -1;
    if (NDF_DSI_LDO_CHAN) ldo_on(NDF_DSI_LDO_CHAN - 1, NDF_DSI_LDO_MV);
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_CSI_BRG_SYS_CLK_EN);                              /* bridge (shared with the ISP in IDF) */
    FSET(HP_SYS_CLKRST_REG_RST_EN_CSI_BRG); FCLR(HP_SYS_CLKRST_REG_RST_EN_CSI_BRG);
    FSET(HP_SYS_CLKRST_REG_CSI_HOST_SYS_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_CSI_HOST); FCLR(HP_SYS_CLKRST_REG_RST_EN_CSI_HOST);
    FSET(HP_SYS_CLKRST_REG_REF_20M_CLK_EN);
    FPUT(HP_SYS_CLKRST_REG_MIPI_CSI_DPHY_CLK_SRC_SEL, 0);                    /* PLL_F20M */
    FCLR(HP_SYS_CLKRST_REG_MIPI_CSI_DPHY_CFG_CLK_EN); FSET(HP_SYS_CLKRST_REG_MIPI_CSI_DPHY_CFG_CLK_EN);
    irq_unlock(t);
    REG_SET(CSI_BRIG_CLK_EN_REG, BIT(CSI_BRIG_CLK_EN_S));
    s.h = c->h_res; s.v = c->v_res; s.bpp = c->bits_per_pixel;
    /* PHY reset, high-speed frequency range through the D-PHY test interface, release */
    HO(PHY_SHUTDOWNZ, 0); HO(DPHY_RSTZ, 0); HO(CSI2_RESETN, 0);              /* inputs are active low: shutdown + reset asserted */
    REG(CSI_HOST_PHY_TEST_CTRL1_REG) = 0;
    REG(CSI_HOST_PHY_TEST_CTRL0_REG) = 1; REG(CSI_HOST_PHY_TEST_CTRL0_REG) = 0;
    uint8_t sel = 0;
    for (unsigned i = 0; i < sizeof rng / sizeof rng[0]; i++) if (c->lane_mbps >= rng[i].lo && c->lane_mbps <= rng[i].hi) { sel = rng[i].sel; break; }
    phy_write(0x44, sel << 1);
    HO(PHY_SHUTDOWNZ, 1); HO(DPHY_RSTZ, 1); HO(CSI2_RESETN, 1);              /* release */
    HO(N_LANES, c->lanes - 1);
    HO(VCX, 1);                                                               /* virtual channel extension off (0 = enabled) */
    HO(SCRAMBLE_ENABLE, 0);
    BR(HADR_NUM, c->h_res); BR(VADR_NUM, c->v_res);
    BR(CSI_BUF_AFULL_THRD, 960);
    BR(BYTE_ENDIAN_ORDER, c->byte_swap);
    BR(DMA_BURST_LEN, 512);
    BR(DATA_TYPE_MIN, c->dt_max ? c->dt_min : 0x12); BR(DATA_TYPE_MAX, c->dt_max ? c->dt_max : 0x2F);
    BR(CSI_HOST_CM_BYPASS, 1); BR(CSI_HOST_CM_EN, 0);                          /* no colour conversion here (isp.h) */
    BR(CSI_DMA_FLOW_CONTROLLER, 0);
    /* channel 1: bridge FIFO -> memory, the bridge is the flow controller (TT_FC 4), contiguous block */
    if (dwg_open(1, DWG_CSI, DWG_MEM, 1) || dwg_set_flow(1, 4, CSI_BRIDGE_MEM)) return -1;
    s.up = true;
    return 0;
}

int csi_capture(void *buf, uint32_t timeout_us)
{
    if (!s.up || !buf || ((uintptr_t)buf & 63)) return -1;
    size_t bytes = (size_t)s.h * s.v * s.bpp / 8;
    cache_dma_wbinv(buf, bytes);
    REG(CSI_BRIG_INT_CLR_REG) = 0x3F;
    dwg_block_t b = { CSI_BRIDGE_MEM, (uint32_t)(uintptr_t)buf, (uint32_t)(bytes / 8), DWG_WIDTH_64, DWG_ITEMS_512, DWG_ITEMS_512, 16, true, false, false, false };
    if (dwg_run(1, &b)) return -1;
    BR(CSI_BRIG_EN, 1);
    int r = dwg_wait(1, timeout_us);
    BR(CSI_BRIG_EN, 0);
    dwg_stop(1);
    cache_dma_wbinv(buf, bytes);
    return r;
}

void csi_stop(void)
{
    BR(CSI_BRIG_EN, 0);
    dwg_stop(1);
}

uint32_t csi_errors(void) { return REG(CSI_BRIG_INT_RAW_REG) & 0x1F; }
