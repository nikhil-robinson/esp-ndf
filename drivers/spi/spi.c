/* GP-SPI (TRM ch. 30). host 2 = SPI2 @0x60024000, host 3 = SPI3 @0x60025000. */
#include "spi.h"
#include "gpio.h"
#include "gdma.h"
#include "clk.h"
#include "xtensa.h"

#define B(h)          ((h) == 2 ? 0x60024000 : 0x60025000)
#define CMD(h)        (B(h) + 0x00)   /* b24 usr (start) b23 update */
#define CTRL(h)       (B(h) + 0x08)
#define CLOCK(h)      (B(h) + 0x0C)   /* b5:0 l b11:6 h b17:12 n b21:18 pre b31 equ_sysclk */
#define USER(h)       (B(h) + 0x10)   /* b0 doutdin b5 tsck_i b6 cs_hold b7 cs_setup b8 rsck_i b9 ck_out_edge b24/25 highpart b27 mosi b28 miso */
#define USER1(h)      (B(h) + 0x14)
#define USER2(h)      (B(h) + 0x18)
#define MS_DLEN(h)    (B(h) + 0x1C)
#define MISC(h)       (B(h) + 0x20)   /* b0 cs0_dis b29 ck_idle_edge b30 cs_keep_active */
#define DMA_CONF(h)   (B(h) + 0x30)   /* b27 rx_ena b28 tx_ena b29-31 fifo resets */
#define INT_CLR(h)    (B(h) + 0x38)
#define INT_RAW(h)    (B(h) + 0x3C)   /* b12 trans_done */
#define W(h, i)       (B(h) + 0x98 + 4 * (i))
#define SLAVE(h)      (B(h) + 0xE0)   /* b1:0 clk_mode b2 clk_mode_13 b26 slave_mode b27 soft_reset */
#define SLAVE1(h)     (B(h) + 0xE4)
#define CLK_GATE(h)   (B(h) + 0xE8)   /* b0 clk_en b1 mst_clk_active b2 mst_clk_sel (1 = 80 MHz PLL) */

#define DONE          BIT(12)
#define FIFO_RST      (BIT(29) | BIT(30) | BIT(31))

typedef struct { uint8_t clk, mosi, miso, cs; } sig_t;
static const sig_t sig[2] = { { 101, 103, 102, 110 }, { 66, 68, 67, 71 } };   /* SPI2, SPI3 */
#define S(h) sig[(h) - 2]

static int dma_ch[2] = { -1, -1 };
static gdma_desc_t tx_d[8][2], rx_d[8][2];      /* per host: up to 8 x 4092 = 32 KB per call */

static void sync_cfg(int h)                     /* push config into the SPI clock domain */
{
    REG_SET(CMD(h), BIT(23));
    while (REG(CMD(h)) & BIT(23)) {}
}

uint32_t spi_master_init(int h, int sclk, int mosi, int miso, int cs, uint32_t hz, int mode)
{
    if ((h != 2 && h != 3) || !hz || 80000000 % hz) return 0;
    uint32_t div = 80000000 / hz, pre = 0, n = 0;
    for (uint32_t p = 1; p <= 16; p++)                           /* 80 MHz / (pre * n), n <= 64 */
        if (div % p == 0 && div / p <= 64) { pre = p; n = div / p; break; }
    if (!pre) return 0;
    periph_enable(h == 2 ? PERIPH_SPI2 : PERIPH_SPI3);
    REG(CLK_GATE(h)) = BIT(0) | BIT(1) | BIT(2);
    REG(SLAVE(h)) = 0;
    REG(CLOCK(h)) = div == 1 ? BIT(31)
                  : (pre - 1) << 18 | (n - 1) << 12 | ((n / 2) - 1) << 6 | (n - 1);
    uint32_t user = BIT(0) | BIT(6) | BIT(7);                     /* full duplex, cs setup/hold */
    if (mode == 1 || mode == 2) user |= BIT(9);                    /* CPHA via output edge */
    REG(USER(h)) = user;
    REG(USER1(h)) = 0; REG(USER2(h)) = 0; REG(CTRL(h)) = 0;
    REG(MISC(h)) = (mode >= 2 ? BIT(29) : 0);                      /* CPOL */
    REG(DMA_CONF(h)) = FIFO_RST; REG(DMA_CONF(h)) = 0;
    sync_cfg(h);
    if (sclk >= 0) { gpio_config(sclk, GPIO_OUTPUT | GPIO_INPUT); gpio_out_signal(sclk, S(h).clk, false); }
    if (mosi >= 0) { gpio_config(mosi, GPIO_OUTPUT | GPIO_INPUT); gpio_out_signal(mosi, S(h).mosi, false); }
    if (miso >= 0) { gpio_config(miso, GPIO_INPUT | GPIO_FORCE); gpio_in_signal(miso, S(h).miso, false); }
    if (cs >= 0)   { gpio_config(cs, GPIO_OUTPUT | GPIO_INPUT); gpio_out_signal(cs, S(h).cs, false); }
    return 80000000 / div;
}

static void start_and_wait(int h)
{
    REG(INT_CLR(h)) = DONE;
    sync_cfg(h);
    REG_SET(CMD(h), BIT(24));
    while (!(REG(INT_RAW(h)) & DONE)) {}
}

RAM_FN static void xfer_pio(int h, const uint8_t *tx, uint8_t *rx, size_t len)
{
    uint32_t words = (len + 3) / 4;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t v = 0;
        if (tx) for (int b = 0; b < 4 && i * 4 + b < len; b++) v |= (uint32_t)tx[i * 4 + b] << (8 * b);
        REG(W(h, i)) = v;
    }
    REG(MS_DLEN(h)) = len * 8 - 1;
    REG_CLR(USER(h), BIT(24) | BIT(25));
    REG_SET(USER(h), BIT(27) | BIT(28));
    REG_CLR(DMA_CONF(h), BIT(27) | BIT(28));
    start_and_wait(h);
    if (rx) for (uint32_t i = 0; i < words; i++) {
        uint32_t v = REG(W(h, i));
        for (int b = 0; b < 4 && i * 4 + b < len; b++) rx[i * 4 + b] = v >> (8 * b);
    }
}

static int xfer_dma(int h, const void *tx, void *rx, size_t len)
{
    int k = h - 2;
    static uint8_t dummy[4] __attribute__((aligned(4)));
    if (dma_ch[k] < 0) {
        if ((dma_ch[k] = gdma_alloc()) < 0) return -1;
        gdma_connect(dma_ch[k], h == 2 ? GDMA_SPI2 : GDMA_SPI3);
    }
    int c = dma_ch[k];
    REG(DMA_CONF(h)) = FIFO_RST; REG(DMA_CONF(h)) = 0;
    /* no tx: send zeros from a 4-byte buffer looped would need a ring; use rx buffer itself */
    const void *src = tx ? tx : (rx ? rx : dummy);
    if (gdma_chain(tx_d[0], 8, (void *)src, len, true) < 0) return -1;   /* > 32 KB: caller splits */
    gdma_out_start(c, tx_d[0]);
    if (rx) {
        gdma_chain(rx_d[0], 8, rx, len, false);
        gdma_in_start(c, rx_d[0]);
    }
    REG(MS_DLEN(h)) = len * 8 - 1;
    REG_SET(USER(h), BIT(27) | (rx ? BIT(28) : 0));
    if (!rx) REG_CLR(USER(h), BIT(28));
    REG(DMA_CONF(h)) = BIT(28) | (rx ? BIT(27) : 0);
    start_and_wait(h);
    REG(DMA_CONF(h)) = 0;
    return 0;
}

int spi_xfer(int h, const void *tx, void *rx, size_t len)
{
    if (!len) return 0;
    if (len <= 64) { xfer_pio(h, tx, rx, len); return 0; }
    const uint8_t *t = tx; uint8_t *r = rx;
    while (len) {
        size_t n = len > 8 * GDMA_DESC_MAX ? 8 * GDMA_DESC_MAX : len;
        if (xfer_dma(h, t, r, n) < 0) return -1;
        if (t) t += n;
        if (r) r += n;
        len -= n;
    }
    return 0;
}

/* ---- slave (register-buffer mode): the slave shifts each MISO byte out of
 * W0.. and overwrites it with the incoming MOSI byte (TRM 30.5; IDF does the same). */
static uint8_t slv_len[2];

int spi_slave_init(int h, int sclk, int mosi, int miso, int cs, int mode)
{
    if (h != 2 && h != 3) return -1;
    periph_enable(h == 2 ? PERIPH_SPI2 : PERIPH_SPI3);
    REG(CLK_GATE(h)) = BIT(0);
    REG(CLOCK(h)) = 0;
    REG(CTRL(h)) = 0;
    REG(SLAVE(h)) = BIT(27);                                      /* soft reset */
    REG(SLAVE(h)) = BIT(26);                                      /* slave mode */
    /* sample/drive edges per mode (TRM table: rsck_i/tsck_i/clk_mode_13) */
    uint32_t user = BIT(0);
    if (mode == 1 || mode == 2) user |= BIT(5) | BIT(8);
    REG(USER(h)) = user;
    REG(MISC(h)) = mode >= 2 ? BIT(29) : 0;
    REG(DMA_CONF(h)) = FIFO_RST; REG(DMA_CONF(h)) = 0;
    if (sclk >= 0) { gpio_config(sclk, GPIO_INPUT | GPIO_FORCE); gpio_in_signal(sclk, S(h).clk, false); }
    if (mosi >= 0) { gpio_config(mosi, GPIO_INPUT | GPIO_FORCE); gpio_in_signal(mosi, S(h).mosi, false); }
    if (cs >= 0)   gpio_in_signal(cs, S(h).cs, false);                /* may share a pad with the master */
    if (miso >= 0) { gpio_config(miso, GPIO_OUTPUT | GPIO_INPUT); gpio_out_signal(miso, S(h).miso, false); }
    return 0;
}

int spi_slave_arm(int h, const void *tx, size_t len)
{
    if (len > 64) return -1;
    const uint8_t *t = tx;
    /* reset, load W0.., then flush the TX prefetch so the shifter reloads W0 */
    REG_SET(SLAVE(h), BIT(27));
    REG_CLR(SLAVE(h), BIT(27));
    for (int i = 0; i < (int)(len + 3) / 4; i++) {
        uint32_t v = 0;
        for (int b = 0; b < 4 && i * 4 + b < (int)len; b++) v |= (uint32_t)t[i * 4 + b] << (8 * b);
        REG(W(h, i)) = v;
    }
    REG_SET(DMA_CONF(h), BIT(30));                                /* buf_afifo_rst */
    REG_CLR(DMA_CONF(h), BIT(30));
    slv_len[h - 2] = len;
    REG(INT_CLR(h)) = DONE;
    REG_SET(CMD(h), BIT(24));
    return 0;
}

int spi_slave_result(int h, void *rx, size_t len)
{
    if (!(REG(INT_RAW(h)) & DONE)) return -1;
    uint8_t *r = rx;
    size_t n = len < slv_len[h - 2] ? len : slv_len[h - 2];
    for (size_t i = 0; i < n; i++) r[i] = REG(W(h, i / 4)) >> (8 * (i & 3));
    return n;
}
