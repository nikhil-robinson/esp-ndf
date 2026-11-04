/* LCD_CAM, camera half (TRM ch. 29), register sequence after IDF hal/cam_hal.c (P4 port of the same block). */
#include "cam.h"
#include "gpio.h"
#include "gdma.h"
#include "clk.h"
#include "sys.h"
#include "cpu.h"

#define LCD_BASE      0x60041000
#define CAM_CTRL      (LCD_BASE + 0x04)  /* b0 stop_en b3:1 vsync filt b4 update b5 byte order b6 bit order b7 line int
                                            b8 vs_eof b16:9 div_num b22:17 div_b b28:23 div_a b30:29 clk_sel */
#define CAM_CTRL1     (LCD_BASE + 0x08)  /* b15:0 rec bytes-1 b21:16 line int num b22 clk_inv b23 vsync filt en
                                            b24 2byte b25 de_inv b26 hsync_inv b27 vsync_inv b28 vh_de_mode b29 start
                                            b30 reset b31 afifo_reset */
#define CAM_RGB_YUV   (LCD_BASE + 0x0C)  /* b31 conv_bypass: 1 = converter ON (as on the LCD side) */
#define SIG_D0        133
#define SIG_PCLK      149
#define SIG_DE        150
#define SIG_VSYNC     152
#define SIG_XCLK      149                /* CAM_CLK out */

extern int lcd_cam_dma;                    /* shared with the LCD half (i80.c): camera = IN half */
#define dma lcd_cam_dma
static gdma_desc_t d[8];
static uint8_t width;

int cam_init(const cam_cfg_t *c)
{
    if (c->bits != 8 && c->bits != 16) return -1;
    periph_enable(PERIPH_LCD_CAM);                         /* shared with the LCD half: no module reset here */
    if (dma < 0) { if ((dma = gdma_alloc()) < 0) return -1; gdma_connect(dma, GDMA_LCD_CAM); }
    width = c->bits;
    uint32_t g = c->xclk_hz ? 160000000u / c->xclk_hz : 8;
    if (g < 2) g = 2;
    if (g > 255) g = 255;
    /* IDF cam_hal_init: no stop signal, VSYNC ends the DMA frame, no inversions, DE mode, converter off */
    REG(CAM_CTRL) = BIT(8) | g << 9 | 3u << 29;
    REG(CAM_CTRL1) = (c->bits == 16 ? BIT(24) : 0) | 0xFFFF;
    REG(CAM_RGB_YUV) = 0;
    REG_SET(CAM_CTRL, BIT(4));
    for (int i = 0; i < c->bits; i++) if (c->data[i] >= 0) gpio_in_signal(c->data[i], SIG_D0 + i, false);
    gpio_in_signal(c->pclk, SIG_PCLK, false);
    gpio_in_signal(c->vsync, SIG_VSYNC, false);
    gpio_in_signal(c->de, SIG_DE, false);
    if (c->xclk >= 0) { gpio_config(c->xclk, GPIO_OUTPUT); gpio_out_signal(c->xclk, SIG_XCLK, false); }
    return 0;
}

int cam_capture(void *buf, size_t max)
{
    if (gdma_chain(d, 8, buf, max, false) < 0) return -1;
    REG_CLR(CAM_CTRL1, BIT(29));
    REG_SET(CAM_CTRL1, BIT(30)); REG_CLR(CAM_CTRL1, BIT(30));   /* cam reset */
    REG_SET(CAM_CTRL1, BIT(31)); REG_CLR(CAM_CTRL1, BIT(31));   /* afifo reset */
    gdma_in_start(dma, d);
    REG_SET(CAM_CTRL, BIT(4));
    REG_SET(CAM_CTRL1, BIT(29));                            /* start */
    return 0;
}

int cam_wait(uint32_t us)
{
    uint32_t t0 = ccount(), lim = us * (sys_cpu_hz() / 1000000);
    for (;;) {
        size_t n = 0; bool eof = false;
        for (int i = 0; i < 8; i++) { n += d[i].length; if (d[i].suc_eof) { eof = true; break; } if (!d[i].next) break; }
        if (eof) { REG_CLR(CAM_CTRL1, BIT(29)); return (int)n; }
        if (ccount() - t0 > lim) { REG_CLR(CAM_CTRL1, BIT(29)); return -1; }
    }
}
