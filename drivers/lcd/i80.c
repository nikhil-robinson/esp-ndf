/* LCD_CAM, LCD half (TRM ch. 29): i80 bus and RGB timing, register sequence as IDF esp_lcd i80/rgb. */
#include "lcd_i80.h"
#include "gpio.h"
#include "gdma.h"
#include "clk.h"
#include "irq.h"
#include "sys.h"
#include "xtensa.h"

#define LCD_BASE      0x60041000
#define LCD_CLOCK     (LCD_BASE + 0x00)  /* b5:0 clkcnt_n b6 equ_sysclk b7 ck_idle b8 ck_out_edge b16:9 div_num
                                            b22:17 div_b b28:23 div_a b30:29 clk_sel (3 = PLL160) b31 clk_en */
#define LCD_RGB_YUV   (LCD_BASE + 0x10)  /* b31 conv_bypass: 1 = converter ON */
#define LCD_USER      (LCD_BASE + 0x14)  /* b12:0 dout cyclelen b13 always_out b19 8bits_order b20 update b23 2byte
                                            b24 dout b25 dummy b26 cmd b27 start b28 reset b30:29 dummy len b31 cmd 2 cycles */
#define LCD_MISC      (LCD_BASE + 0x18)  /* b5:1 afifo thr b11:6 vfk b24:12 vbk b25 next_frame b26 bk_en b27 afifo_rst
                                            b28 cd_data_set b29 cd_dummy_set b30 cd_cmd_set b31 cd_idle_edge */
#define LCD_CTRL      (LCD_BASE + 0x1C)  /* b10:0 hb_front b20:11 va_height b30:21 vt_height b31 rgb_mode */
#define LCD_CTRL1     (LCD_BASE + 0x20)  /* b7:0 vb_front b19:8 ha_width b31:20 ht_width */
#define LCD_CTRL2     (LCD_BASE + 0x24)  /* b6:0 vsync_w b7 vsync_idle b8 de_idle b9 hs_blank_en b22:16 hsync_w
                                            b23 hsync_idle b31:24 hsync_pos */
#define LCD_CMD_VAL   (LCD_BASE + 0x28)
#define INT_ENA       (LCD_BASE + 0x64)  /* b0 lcd vsync b1 trans done */
#define INT_RAW       (LCD_BASE + 0x68)
#define INT_ST        (LCD_BASE + 0x6C)
#define INT_CLR       (LCD_BASE + 0x70)
#define SIG_DATA0     133
#define SIG_CS        132
#define SIG_DE        150
#define SIG_HSYNC     151
#define SIG_VSYNC     152
#define SIG_DC        153
#define SIG_PCLK      154
#define SRC_LCD_CAM   24

int lcd_cam_dma = -1;                       /* one GDMA channel: LCD uses its OUT half, the camera its IN half */
#define dma lcd_cam_dma
static int line = -1;
static uint8_t width;
static gdma_desc_t d[9];
static volatile bool busy;
static volatile uint32_t frames;

RAM_FN static void lcd_isr(void *a)
{
    (void)a;
    uint32_t st = REG(INT_ST);
    REG(INT_CLR) = st;
    if (st & BIT(1)) busy = false;
    if (st & BIT(0)) frames++;
}

/* PLL160 / group divider (2..255) / pixel prescaler (1..64) */
static uint32_t clock_set(uint32_t hz)
{
    uint32_t tot = (160000000u + hz / 2) / hz, g = (tot + 63) / 64, p;
    if (g < 2) g = 2;
    if (g > 255) return 0;
    p = (tot + g / 2) / g;
    if (p < 1) p = 1;
    if (p > 64) p = 64;
    REG(LCD_CLOCK) = (p == 1 ? BIT(6) : (p - 1)) | BIT(7) | g << 9 | 3u << 29 | BIT(31);   /* pclk idles high */
    return 160000000u / g / p;
}

static int bus_up(uint8_t bits)
{
    if (bits != 8 && bits != 16) return -1;
    periph_enable(PERIPH_LCD_CAM);
    periph_reset(PERIPH_LCD_CAM);
    if (dma < 0) { if ((dma = gdma_alloc()) < 0) return -1; }
    gdma_connect(dma, GDMA_LCD_CAM);
    if (line < 0 && (line = irq_attach(SRC_LCD_CAM, 1, lcd_isr, 0)) < 0) return -1;
    width = bits;
    REG(LCD_RGB_YUV) = 0;                                   /* conv_bypass = 0: converter off (1 enables it) */
    REG_SET(LCD_USER, BIT(28));                             /* lcd_reset (self clearing) */
    REG_SET(LCD_MISC, BIT(27));                             /* afifo reset */
    REG(INT_CLR) = ~0u;
    return 0;
}

static void route(const int8_t *data, int n)
{
    for (int i = 0; i < n; i++) if (data[i] >= 0) {
        gpio_config(data[i], GPIO_OUTPUT | GPIO_INPUT);
        gpio_out_signal(data[i], SIG_DATA0 + i, false);
    }
}

static void out(int pin, int sig) { if (pin >= 0) { gpio_config(pin, GPIO_OUTPUT | GPIO_INPUT); gpio_out_signal(pin, sig, false); } }

uint32_t lcd_i80_init(const lcd_i80_cfg_t *c)
{
    if (bus_up(c->bits) < 0) return 0;
    uint32_t hz = clock_set(c->pclk_hz);
    if (!hz) return 0;
    REG(LCD_CTRL) = 0;                                      /* i80, not RGB */
    /* IDF defaults: DC 0 idle and command, 1 for data; DMA reads 1 or 2 bytes per WR; output always on */
    REG(LCD_MISC) = BIT(28) | 17u << 1;                     /* afifo threshold: reset default 17 */
    REG(LCD_USER) = (c->bits == 16 ? BIT(23) : 0) | BIT(13);
    route(c->data, c->bits);
    out(c->wr, SIG_PCLK); out(c->dc, SIG_DC); out(c->cs, SIG_CS);
    /* IDF lcd_periph_trigger_quick_trans_done_event: one dummy cycle sets the bus to its idle levels */
    uint32_t u = REG(LCD_USER) & (BIT(23) | BIT(13));
    REG(INT_CLR) = BIT(1);
    REG(LCD_USER) = u | BIT(25) | BIT(20);
    REG(LCD_USER) = u | BIT(25) | BIT(20) | BIT(27);
    uint32_t t = ccount();
    while (!(REG(INT_RAW) & BIT(1)) && ccount() - t < 24000000u) {}
    REG(LCD_USER) = u;
    busy = false;
    REG(INT_CLR) = BIT(1);
    REG_SET(INT_ENA, BIT(1));
    return hz;
}

int lcd_i80_send_async(int cmd, const void *data, size_t n)
{
    if (gdma_is_psram(data)) return -1;                          /* TX DMA from PSRAM: refused (gdma.h) */
    lcd_i80_wait();
    if (n && gdma_chain(d, 8, (void *)data, n, true) < 0) return -1;
    uint32_t u = REG(LCD_USER) & (BIT(23) | BIT(13));
    if (cmd >= 0) {
        REG(LCD_CMD_VAL) = width == 8 ? ((uint32_t)cmd & 0xFF) | ((uint32_t)cmd & 0xFF00) << 8 : (uint32_t)cmd;
        u |= BIT(26);
    }
    if (n) u |= BIT(24);                                    /* data cycles follow the DMA length */
    REG(LCD_USER) = u;
    REG_FIELD(LCD_MISC, 26, 1, 1);                          /* blank cycles 1 / 1 (IDF) */
    REG_FIELD(LCD_MISC, 6, 6, 0); REG_FIELD(LCD_MISC, 12, 13, 0);
    REG_SET(LCD_MISC, BIT(27));                             /* afifo reset */
    busy = true;
    if (n) { gdma_out_start(dma, d); delay_us(4); }         /* IDF: let DMA prefill the FIFO */
    REG(LCD_USER) = u | BIT(20);
    REG(LCD_USER) = u | BIT(20) | BIT(27);                  /* update + start */
    return 0;
}

void lcd_i80_wait(void) { uint32_t t = ccount(); while (busy && ccount() - t < 240000000u) {} busy = false; }

int lcd_i80_send(int cmd, const void *data, size_t n)
{
    int r = lcd_i80_send_async(cmd, data, n);
    lcd_i80_wait();
    return r;
}

/* ---- RGB ---- */
static gdma_desc_t fbd[64];

uint32_t lcd_rgb_start(const lcd_rgb_cfg_t *c, const void *fb)
{
    if (gdma_is_psram(fb)) return 0;                          /* TX DMA from PSRAM: refused (gdma.h) */
    if (bus_up(c->bits) < 0) return 0;
    uint32_t hz = clock_set(c->pclk_hz);
    size_t bytes = (size_t)c->width * c->height * (c->bits / 8);
    int k = gdma_chain(fbd, 64, (void *)fb, bytes, false);
    if (!hz || k < 0) return 0;
    fbd[k - 1].next = &fbd[0];                              /* the frame buffer for ever */
    REG(LCD_USER) = (c->bits == 16 ? BIT(23) : 0) | BIT(13) | BIT(24);
    REG(LCD_MISC) = 17u << 1 | BIT(25) | BIT(26);           /* next frame automatically, porches (IDF blank 1/1) */
    /* IDF lcd_ll_set_horizontal/vertical_timing */
    REG(LCD_CTRL) = (uint32_t)(c->hbp + c->hsw - 1) | (uint32_t)(c->height - 1) << 11 |
                    (uint32_t)(c->vsw + c->vbp + c->height + c->vfp - 1) << 21 | BIT(31);
    REG(LCD_CTRL1) = (uint32_t)(c->vbp + c->vsw - 1) | (uint32_t)(c->width - 1) << 8 |
                     (uint32_t)(c->hsw + c->hbp + c->width + c->hfp - 1) << 20;
    REG(LCD_CTRL2) = (uint32_t)(c->vsw - 1) | BIT(7) | BIT(9) | (uint32_t)(c->hsw - 1) << 16 | BIT(23);   /* syncs idle high */
    route(c->data, c->bits);
    out(c->pclk, SIG_PCLK); out(c->hsync, SIG_HSYNC); out(c->vsync, SIG_VSYNC); out(c->de, SIG_DE);
    frames = 0;
    REG(INT_CLR) = BIT(0);
    REG_SET(INT_ENA, BIT(0));
    REG_SET(LCD_MISC, BIT(27));
    gdma_out_start(dma, fbd);
    delay_us(4);
    REG(LCD_USER) |= BIT(20);
    REG(LCD_USER) |= BIT(27);
    return hz;
}

void lcd_rgb_stop(void)
{
    REG_CLR(LCD_MISC, BIT(25));
    REG_CLR(LCD_USER, BIT(27));
    REG_CLR(INT_ENA, BIT(0));
    if (dma >= 0) gdma_stop(dma);
}

uint32_t lcd_rgb_frames(void) { return frames; }

void lcd_cam_release(void)                     /* stop both halves, give the GDMA channel back */
{
    lcd_rgb_stop();
    REG_CLR(INT_ENA, BIT(0) | BIT(1));
    if (dma >= 0) { gdma_stop(dma); gdma_free(dma); dma = -1; }
}
