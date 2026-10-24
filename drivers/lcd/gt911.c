/* GT911: 16-bit register addresses, big-endian on the wire; report at 0x814E (b7 ready, b3:0 points),
 * points from 0x8150, 8 bytes each (id, x, y, size little-endian); write 0 to 0x814E to release it. */
#include "gt911.h"
#include "i2c.h"

static int g_port; static uint8_t g_addr;

static int rd(uint16_t reg, uint8_t *b, size_t n)
{
    uint8_t a[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_xfer(g_port, g_addr, a, 2, b, n, 20000) == I2C_OK ? 0 : -1;
}

static int wr8(uint16_t reg, uint8_t v)
{
    uint8_t a[3] = { reg >> 8, reg & 0xFF, v };
    return i2c_master_xfer(g_port, g_addr, a, 3, 0, 0, 20000) == I2C_OK ? 0 : -1;
}

int gt911_init(int port, uint8_t addr, char id[5], uint16_t *xres, uint16_t *yres)
{
    g_port = port; g_addr = addr;
    uint8_t b[8];
    if (rd(0x8140, b, 8)) return -1;                       /* product id (4 ascii), fw version, x res, y res */
    if (id) { for (int i = 0; i < 4; i++) id[i] = b[i]; id[4] = 0; }
    if (xres) *xres = b[6] | b[7] << 8;
    uint8_t y[2];
    if (rd(0x8148, y, 2)) return -1;
    if (yres) *yres = y[0] | y[1] << 8;
    return wr8(0x814E, 0);
}

int gt911_read(gt911_point_t *p, int max)
{
    uint8_t st;
    if (rd(0x814E, &st, 1)) return -1;
    if (!(st & 0x80)) return 0;                            /* no new report */
    int n = st & 0x0F;
    if (n > 5) n = 5;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        uint8_t b[8];
        if (rd(0x8150 + 8 * i, b, 8)) return -1;
        p[i] = (gt911_point_t){ .id = b[0], .x = b[1] | b[2] << 8, .y = b[3] | b[4] << 8, .size = b[5] | b[6] << 8 };
    }
    wr8(0x814E, 0);
    return n;
}
