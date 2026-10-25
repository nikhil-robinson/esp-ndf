/* LCD over SPI. D/C low = command byte, D/C high = parameters / pixels. */
#include "lcd.h"
#include "spi.h"
#include "gpio.h"
#include "ledc.h"
#include "sys.h"

static lcd_cfg_t cfg;
#define LINES 20                                       /* DMA chunk: 20 lines x 320 px x 2 B = 12.8 KB */
static uint16_t fillbuf[320 * LINES] __attribute__((aligned(4)));

void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t n)
{
    gpio_clr(cfg.dc);
    spi_xfer(cfg.host, &cmd, 0, 1);
    if (n) { gpio_set(cfg.dc); spi_xfer(cfg.host, data, 0, n); }
}

static void window(int x0, int y0, int x1, int y1)
{
    uint8_t ca[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF }, ra[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    lcd_cmd(0x2A, ca, 4);
    lcd_cmd(0x2B, ra, 4);
    lcd_cmd(0x2C, 0, 0);
}

void lcd_draw(int x0, int y0, int x1, int y1, const uint16_t *px)
{
    window(x0, y0, x1, y1);
    gpio_set(cfg.dc);
    spi_xfer(cfg.host, px, 0, (size_t)(x1 - x0 + 1) * (y1 - y0 + 1) * 2);
}

void lcd_fill(uint16_t color)
{
    for (int i = 0; i < cfg.width * LINES; i++) fillbuf[i] = color;
    window(0, 0, cfg.width - 1, cfg.height - 1);
    gpio_set(cfg.dc);
    for (int y = 0; y < cfg.height; y += LINES) {
        int n = cfg.height - y < LINES ? cfg.height - y : LINES;
        spi_xfer(cfg.host, fillbuf, 0, (size_t)cfg.width * n * 2);
    }
}

void lcd_backlight(int pct)
{
    if (cfg.bl < 0) return;
    ledc_duty(7, (uint32_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct) * 1023 / 100);
}

int lcd_init(const lcd_cfg_t *c)
{
    cfg = *c;
    spi_master_init(c->host, c->sclk, c->mosi, -1, c->cs, c->pclk_hz, 0);
    gpio_config(c->dc, GPIO_OUTPUT);
    if (c->bl >= 0) { ledc_timer(3, 5000, 10); ledc_channel(7, 3, c->bl, 0); }
    if (c->rst >= 0) {                                  /* hardware reset pulse */
        gpio_config(c->rst, GPIO_OUTPUT);
        gpio_write(c->rst, c->rst_active_high);
        delay_us(10000);
        gpio_write(c->rst, !c->rst_active_high);
        delay_us(120000);
    } else {
        lcd_cmd(0x01, 0, 0);                            /* software reset */
        delay_us(120000);
    }
    for (const lcd_init_cmd_t *i = c->init; i && !(i->cmd == 0 && i->len == 0xFF); i++) {
        lcd_cmd(i->cmd, i->data, i->len & 0x7F);
        if (i->len & 0x80) delay_us(120000);            /* 0x80 flag: sleep-out / display-on settle */
        if (i->delay_ms) delay_us(i->delay_ms * 1000);
    }
    lcd_cmd(0x36, &c->madctl, 1);
    return 0;
}
