/* P4 ISP (isp.h). Flow of ESP-IDF v6.1 esp_driver_isp/src/isp_core.c and esp_hal_cam/esp32p4/isp_ll.h: ISP module clock
 * (source PLL_F240M, integer divider) + reset, block clock, error interrupts, input from the CSI bridge, input / output colour
 * type with the demosaic / RGB<->YUV stages that the type needs, frame size minus one, Bayer order, YUV standard and range,
 * shadow update on the next vsync. Registers: generated regs/isp.h. NOT VERIFIED ON HARDWARE. */
#include "isp.h"
#include "clk.h"
#include "cpu.h"
#include "soc.h"
#include "regs/isp.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"

#define I(F, v)  FPUT(ISP_##F, v)
#define IS(F)    FSET(ISP_##F)
#define IC(F)    FCLR(ISP_##F)
enum { SHADOW_DISABLE, SHADOW_EVERY_VSYNC, SHADOW_NEXT_VSYNC };

int isp_init(const isp_cfg_t *c)
{
    if (!c || c->h_res < 1 || c->v_res < 1 || c->h_res > 4096 || c->v_res > 4096 || c->clk_khz < 1000 || (unsigned)c->in > ISP_IN_RAW12 || (unsigned)c->out > ISP_OUT_YUV420) return -1;
    uint32_t div = (240000 + c->clk_khz / 2) / c->clk_khz;
    if (div < 1 || div > 256) return -1;
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_REF_240M_CLK_EN);
    FPUT(HP_SYS_CLKRST_REG_ISP_CLK_SRC_SEL, 1);                               /* PLL_F240M */
    FPUT(HP_SYS_CLKRST_REG_ISP_CLK_DIV_NUM, div - 1);
    FSET(HP_SYS_CLKRST_REG_ISP_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_ISP); FCLR(HP_SYS_CLKRST_REG_RST_EN_ISP);
    irq_unlock(t);
    I(CLK_EN, 1);
    REG(ISP_INT_CLR_REG) = 0xFFFFFFFFu;
    REG(ISP_INT_ENA_REG) = 0x3F;                                              /* error events (data type, FIFO, buffer, size) */
    I(ISP_IN_SRC, 0); I(MIPI_DATA_EN, 1);                                     /* CSI bridge */
    I(HSYNC_START_EXIST, c->csi_has_line_packets); I(HSYNC_END_EXIST, c->csi_has_line_packets);
    I(ISP_DATA_TYPE, c->in);                                                  /* RAW8 / 10 / 12 */
    static const uint8_t out_type[5] = { 0, 2, 4, 1, 3 };
    static const bool demosaic[5] = { 0, 1, 1, 1, 1 }, r2y[5] = { 0, 1, 1, 1, 1 }, y2r[5] = { 0, 1, 1, 0, 0 };   /* ISP_OUT_RGB* run through YUV and back */
    I(ISP_OUT_TYPE, out_type[c->out]);
    I(DEMOSAIC_EN, demosaic[c->out]); I(RGB2YUV_EN, r2y[c->out]); I(YUV2RGB_EN, y2r[c->out]);
    uint32_t bits = c->in == ISP_IN_RAW8 ? 8 : c->in == ISP_IN_RAW10 ? 10 : 12;
    (void)bits;
    I(HADR_NUM, c->h_res - 1); I(VADR_NUM, c->v_res - 1);
    I(BAYER_MODE, c->bayer);
    I(YUV_MODE, c->yuv_bt709); I(YUV_RANGE, !c->yuv_full);                      /* register 1 = limited range */
    I(COLOR_EN, 1);                                                           /* IDF workaround DIG-474: colour block on */
    I(BYTE_ENDIAN_ORDER, c->byte_swap);
    I(SHADOW_UPDATE_SEL, SHADOW_NEXT_VSYNC);
    return 0;
}

void isp_start(void) { IS(ISP_EN); }
void isp_stop(void)  { IC(ISP_EN); }
void isp_update(void)
{
    REG_SET(ISP_SHADOW_REG_CTRL_REG, BIT(ISP_BLC_UPDATE_S) | BIT(ISP_WBG_UPDATE_S) | BIT(ISP_CCM_UPDATE_S) | BIT(ISP_SHARP_UPDATE_S) | BIT(ISP_COLOR_UPDATE_S));
}

void isp_ccm(const int16_t m[3][3])
{
    I(CCM_RR, m[0][0] & 0x1FFF); I(CCM_RG, m[0][1] & 0x1FFF); I(CCM_RB, m[0][2] & 0x1FFF);
    I(CCM_GR, m[1][0] & 0x1FFF); I(CCM_GG, m[1][1] & 0x1FFF); I(CCM_GB, m[1][2] & 0x1FFF);
    I(CCM_BR, m[2][0] & 0x1FFF); I(CCM_BG, m[2][1] & 0x1FFF); I(CCM_BB, m[2][2] & 0x1FFF);
}
void isp_ccm_enable(bool on) { on ? IS(CCM_EN) : IC(CCM_EN); }
void isp_color(uint8_t s, uint8_t h, uint8_t c, uint8_t b)
{
    I(COLOR_SATURATION, s); I(COLOR_HUE, h); I(COLOR_CONTRAST, c); I(COLOR_BRIGHTNESS, b);
    IS(COLOR_EN);
}
void isp_demosaic_grad_ratio(uint8_t r) { I(DEMOSAIC_GRAD_RATIO, r & 0x3F); }
void isp_sharpen_enable(bool on) { on ? IS(SHARP_EN) : IC(SHARP_EN); }
void isp_blc_enable(bool on)     { on ? IS(BLC_EN) : IC(BLC_EN); }
void isp_wbg_enable(bool on)     { on ? IS(WBG_EN) : IC(WBG_EN); }
void isp_crop_enable(bool on)    { on ? IS(CROP_EN) : IC(CROP_EN); }
uint32_t isp_errors(void) { return REG(ISP_INT_RAW_REG) & 0x3F; }
