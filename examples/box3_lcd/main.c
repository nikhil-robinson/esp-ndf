/* ESP32-S3-BOX-3 display demo: color bars, gradient, moving block.
 * Pins from Espressif's esp-bsp esp-box-3.h. Panel ILI9342C (ILI9341 family). */
#include "lcd.h"
#include "console.h"
#include "sys.h"
#include "xtensa.h"

static const lcd_init_cmd_t box3_init[] = {           /* vendor sequence, as the BSP sends it */
    { 0xC8, 3, 0, { 0xFF, 0x93, 0x42 } },
    { 0xC0, 2, 0, { 0x0E, 0x0E } },
    { 0xC5, 1, 0, { 0xD0 } },
    { 0xC1, 1, 0, { 0x02 } },
    { 0xB4, 1, 0, { 0x02 } },
    { 0xE0, 15, 0, { 0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F } },
    { 0xE1, 15, 0, { 0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F } },
    { 0xB1, 2, 0, { 0x00, 0x1B } },
    { 0x3A, 1, 0, { 0x55 } },                          /* 16 bit/pixel */
    { 0xB7, 1, 0, { 0x06 } },
    { 0x11, 0x80, 0, { 0 } },                          /* sleep out (+120 ms) */
    { 0x29, 0x80, 0, { 0 } },                          /* display on */
    { 0, 0xFF, 0, { 0 } },
};

static uint16_t line[320 * 20] __attribute__((aligned(4)));

int main(void)
{
    lcd_cfg_t c = { .host = 2, .sclk = 7, .mosi = 6, .cs = 5, .dc = 4, .rst = 48, .bl = 47,
                    .pclk_hz = 40000000, .rst_active_high = true, .width = 320, .height = 240,
                    .madctl = 0x08 | 0x40 | 0x80, .init = box3_init };   /* BGR + mirror x/y (BSP) */
    printf("\nbox3_lcd: init\n");
    lcd_init(&c);
    lcd_backlight(100);

    static const uint8_t bars[8][3] = { {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
                                        {255,0,255}, {255,0,0}, {0,0,255}, {0,0,0} };
    uint32_t t = ccount();
    for (int y = 0; y < 240; y += 20) {                /* 8 vertical color bars */
        for (int r = 0; r < 20; r++) for (int x = 0; x < 320; x++)
            line[r * 320 + x] = lcd_rgb(bars[x / 40][0], bars[x / 40][1], bars[x / 40][2]);
        lcd_draw(0, y, 319, y + 19, line);
    }
    printf("box3_lcd: color bars drawn in %lu us (320x240 RGB565 @40 MHz, wire 30720 us)\n", (ccount() - t) / 240);
    delay_us(3000000);
    t = ccount();
    lcd_fill(lcd_rgb(255, 0, 0));
    printf("box3_lcd: full-screen fill in %lu us\n", (ccount() - t) / 240);
    delay_us(1000000);
    for (int y = 0; y < 240; y += 20) {                /* horizontal gradient: black -> blue */
        for (int r = 0; r < 20; r++) for (int x = 0; x < 320; x++) line[r * 320 + x] = lcd_rgb(0, (y + r), x * 255 / 319);
        lcd_draw(0, y, 319, y + 19, line);
    }
    printf("box3_lcd: gradient drawn; now bouncing a white square\n");
    int x = 0, y = 0, dx = 3, dy = 2;
    for (int i = 0; i < 32 * 32; i++) line[i] = lcd_rgb(255, 255, 255);
    for (int f = 0; f < 600; f++) {
        lcd_draw(x, y, x + 31, y + 31, line);
        x += dx; y += dy;
        if (x < 0 || x > 320 - 32) { dx = -dx; x += 2 * dx; }
        if (y < 0 || y > 240 - 32) { dy = -dy; y += 2 * dy; }
        delay_us(16000);
    }
    printf("box3_lcd: done\n");
    return 0;
}
