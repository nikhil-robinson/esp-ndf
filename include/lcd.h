/* LCD: SPI panel I/O (command/parameter bytes with a D/C pin, pixel data by
 * DMA) + ILI9341-family panel driver. RGB565, big-endian on the wire. */
#pragma once
#include "ndf.h"

typedef struct { uint8_t cmd; uint8_t len; uint8_t delay_ms; uint8_t data[15]; } lcd_init_cmd_t;

typedef struct {
    int host, sclk, mosi, cs, dc, rst, bl;
    uint32_t pclk_hz;
    bool rst_active_high;
    uint16_t width, height;
    uint8_t madctl;                    /* memory access control (mirror/BGR) */
    const lcd_init_cmd_t *init;        /* vendor sequence, terminated by cmd 0 / len 0xFF */
} lcd_cfg_t;

int  lcd_init(const lcd_cfg_t *c);                     /* returns 0 */
void lcd_backlight(int percent);
void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t n);
/* pixels: (x1-x0+1)*(y1-y0+1) RGB565 values, already byte-swapped (lcd_rgb) */
void lcd_draw(int x0, int y0, int x1, int y1, const uint16_t *px);
void lcd_fill(uint16_t color);
static inline uint16_t lcd_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3;
    return c >> 8 | c << 8;                            /* wire order: high byte first */
}
