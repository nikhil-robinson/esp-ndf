/* GP-SPI (TRM ch. 30). host 2 = SPI2 @0x60024000, host 3 = SPI3 @0x60025000. */
#include "spi.h"
#include "spin.h"
#include "gpio.h"
#include "gdma.h"
#include "clk.h"
#include "xtensa.h"

#define B(h)          ((h) == 2 ? 0x60024000 : 0x60025000)
#define CMD(h)        (B(h) + 0x00)   /* b24 usr (start) b23 update */
#define ADDR(h)       (B(h) + 0x04)
#define CTRL(h)       (B(h) + 0x08)   /* b5-7 faddr dual/quad/oct b8-10 fcmd b14-16 fread */
#define CLOCK(h)      (B(h) + 0x0C)   /* b5:0 l b11:6 h b17:12 n b21:18 pre b31 equ_sysclk */
#define USER(h)       (B(h) + 0x10)   /* b0 doutdin b5 tsck_i b6 cs_hold b7 cs_setup b8 rsck_i b9 ck_out_edge b24/25 highpart b27 mosi b28 miso */
#define USER1(h)      (B(h) + 0x14)   /* b7:0 dummy-1 b21:17 cs_setup b26:22 cs_hold b31:27 addr bits-1 */
#define USER2(h)      (B(h) + 0x18)   /* b15:0 cmd value b31:28 cmd bits-1 */
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
#define WR_BUF_DONE   BIT(11)
#define RD_BUF_DONE   BIT(10)
#define FIFO_RST      (BIT(29) | BIT(30) | BIT(31))

typedef struct { uint8_t clk, mosi, miso, cs; uint8_t io[8]; } sig_t;
static const sig_t sig[2] = { { 101, 103, 102, 110, { 103, 102, 105, 104, 106, 107, 108, 109 } },   /* SPI2: D Q WP HD IO4-7 */
                              { 66, 68, 67, 71, { 68, 67, 70, 69, 0, 0, 0, 0 } } };            /* SPI3: quad only */
#define S(h) sig[(h) - 2]

static int dma_ch[2] = { -1, -1 };
static uint32_t user_base[2], user1_base[2];                     /* USER value set at init (mode, edges, cs timing) */
static bool dma_used[2], hd_on[2];                          /* last transfer left DMA_CONF set */
static gdma_desc_t tx_d[8][2], rx_d[8][2];      /* per host: up to 8 x 4092 = 32 KB per call */

static void sync_cfg(int h)                     /* push config into the SPI clock domain */
{
    REG(CMD(h)) = BIT(23);                      /* CMD holds only update/usr: write, no read-modify-write */
    SPIN_US(!(REG(CMD(h)) & BIT(23)), 1000);
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
    user_base[h - 2] = user & ~(BIT(24) | BIT(25) | BIT(27) | BIT(28));
    REG(USER1(h)) = 0; REG(USER2(h)) = 0; REG(CTRL(h)) = 0; user1_base[h - 2] = 0;
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
    SPIN_US(!(REG(INT_RAW(h)) & DONE), 1000);   /* the clear is a posted write: a stale DONE would end the wait early */
    sync_cfg(h);
    REG(CMD(h)) = BIT(24);
    SPIN_US(REG(INT_RAW(h)) & DONE, 10000000u);  /* bounded: 10 s (32 KB at the slowest clock fits) */
}

RAM_FN static void xfer_pio(int h, const uint8_t *tx, uint8_t *rx, size_t len, uint32_t user)
{
    uint32_t words = (len + 3) / 4;
    if (tx && !((uintptr_t)tx & 3) && !(len & 3)) {
        for (uint32_t i = 0; i < words; i++) REG(W(h, i)) = ((const uint32_t *)tx)[i];
    } else {
        for (uint32_t i = 0; i < words; i++) {
            uint32_t v = 0;
            if (tx) for (int b = 0; b < 4 && i * 4 + b < len; b++) v |= (uint32_t)tx[i * 4 + b] << (8 * b);
            REG(W(h, i)) = v;
        }
    }
    REG(MS_DLEN(h)) = len ? len * 8 - 1 : 0;
    REG(USER(h)) = user;                                    /* absolute write, no read-modify-write */
    if (dma_used[h - 2]) { REG(DMA_CONF(h)) = 0; dma_used[h - 2] = false; }
    start_and_wait(h);
    if (rx && !((uintptr_t)rx & 3) && !(len & 3)) {
        for (uint32_t i = 0; i < words; i++) ((uint32_t *)rx)[i] = REG(W(h, i));
    } else if (rx) for (uint32_t i = 0; i < words; i++) {
        uint32_t v = REG(W(h, i));
        for (int b = 0; b < 4 && i * 4 + b < len; b++) rx[i * 4 + b] = v >> (8 * b);
    }
}

static int xfer_dma(int h, const void *tx, void *rx, size_t len, uint32_t user, bool hd)
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
    bool out = tx || !hd;                                 /* full duplex always clocks something out */
    if (out) {
        if (gdma_chain(tx_d[0], 8, (void *)src, len, true) < 0) return -1;   /* > 32 KB: caller splits */
        gdma_out_start(c, tx_d[0]);
    }
    if (rx) {
        gdma_chain(rx_d[0], 8, rx, len, false);
        gdma_in_start(c, rx_d[0]);
    }
    REG(MS_DLEN(h)) = len * 8 - 1;
    REG(USER(h)) = user;
    REG(DMA_CONF(h)) = (out ? BIT(28) : 0) | (rx ? BIT(27) : 0);
    start_and_wait(h);
    REG(DMA_CONF(h)) = 0;
    return 0;
}

static void hd_off(int h)                               /* back to plain full duplex after spi_hd_xfer */
{
    REG(CTRL(h)) = 0; REG(USER1(h)) = user1_base[h - 2]; REG(USER2(h)) = 0;
    REG_CLR(MISC(h), 0xFu << 16);                       /* DTR off */
    hd_on[h - 2] = false;
}

int spi_xfer(int h, const void *tx, void *rx, size_t len)
{
    if (!len) return 0;
    if (hd_on[h - 2]) hd_off(h);
    uint32_t fd = user_base[h - 2] | BIT(27) | BIT(28);
    if (len <= 64) { xfer_pio(h, tx, rx, len, fd); return 0; }
    const uint8_t *t = tx; uint8_t *r = rx;
    while (len) {
        size_t n = len > 8 * GDMA_DESC_MAX ? 8 * GDMA_DESC_MAX : len;
        if (xfer_dma(h, t, r, n, user_base[h - 2] | BIT(27) | (r ? BIT(28) : 0), false) < 0) return -1;
        if (t) t += n;
        if (r) r += n;
        len -= n;
    }
    return 0;
}

int spi_master_io(int h, int line, int pin)
{
    if ((h != 2 && h != 3) || line < 0 || line > (h == 2 ? 7 : 3)) return -1;
    gpio_config(pin, GPIO_OUTPUT | GPIO_INPUT);
    gpio_out_signal(pin, S(h).io[line], false);
    gpio_in_signal(pin, S(h).io[line], false);
    return 0;
}

void spi_master_cs_timing(int h, uint32_t setup, uint32_t hold)
{
    /* cs_setup/cs_hold enable bits are on from init; the time fields add (n) extra SCLK cycles */
    user1_base[h - 2] = (setup > 31 ? 31 : setup) << 17 | (hold > 31 ? 31 : hold) << 22;
    REG(USER1(h)) = user1_base[h - 2];
}

static uint32_t lines_sh(uint8_t n) { return n == 2 ? 0 : n == 4 ? 1 : n == 8 ? 2 : 3; }   /* 3: one line */

int spi_hd_xfer(int h, const spi_hd_t *t, const void *tx, void *rx, size_t len)
{
    if ((h != 2 && h != 3) || (tx && rx) || t->cmd_bits > 16 || t->addr_bits > 32 ||
        (h == 3 && (t->lines == 8 || t->cmd_lines == 8 || t->addr_lines == 8))) return -1;
    int k = h - 2;
    uint32_t dl = lines_sh(t->lines), al = lines_sh(t->addr_lines), cl = lines_sh(t->cmd_lines);
    REG(CTRL(h)) = (al < 3 ? BIT(5 + al) : 0) | (cl < 3 ? BIT(8 + cl) : 0) | (rx && dl < 3 ? BIT(14 + dl) : 0);
    REG(USER1(h)) = user1_base[k] | (t->addr_bits ? (t->addr_bits - 1u) << 27 : 0) | (t->dummy ? t->dummy - 1u : 0);
    /* MSB first: command bits go out from bit 7 of the value (IDF HAL_SPI_SWAP_DATA_TX), address from bit 31 */
    REG(USER2(h)) = (t->cmd_bits ? (t->cmd_bits - 1u) << 28 : 0) |
                    (t->cmd_bits ? __builtin_bswap32((uint32_t)t->cmd << (32 - t->cmd_bits)) & 0xFFFF : 0);
    if (t->addr_bits) REG(ADDR(h)) = t->addr_bits == 32 ? t->addr : t->addr << (32 - t->addr_bits);
    REG(MISC(h)) = (REG(MISC(h)) & ~(0xFu << 16)) | (t->dtr ? BIT(16) : 0) |   /* clk_data_dtr + per phase */
                   (t->dtr & SPI_DTR_DATA ? BIT(17) : 0) | (t->dtr & SPI_DTR_ADDR ? BIT(18) : 0) | (t->dtr & SPI_DTR_CMD ? BIT(19) : 0);
    hd_on[k] = true;
    uint32_t user = (user_base[k] & ~BIT(0)) | (tx && dl < 3 ? BIT(12 + dl) : 0) |
                    (t->cmd_bits ? BIT(31) : 0) | (t->addr_bits ? BIT(30) : 0) | (t->dummy ? BIT(29) : 0) |
                    (tx && len ? BIT(27) : 0) | (rx && len ? BIT(28) : 0);
    if (len <= 64) { xfer_pio(h, tx, rx, len, user); return 0; }
    if (len > 8 * GDMA_DESC_MAX) return -1;               /* one phase sequence per call: <= 32 KB */
    return xfer_dma(h, tx, rx, len, user, true);
}

/* ---- slave half duplex, buffer mode (IDF spi_slave_hd, TRM 30.5.2): the master sends an 8-bit command,
 * 8-bit address and 8 dummy cycles; WRBUF (0x01) writes W0.. from that byte address, RDBUF (0x02) reads
 * them. Command bits 0x10/0x20 (+0x40/0x80 for dual/quad address) select 2/4 data lines. */
int spi_slave_hd_init(int h, int sclk, int cs, const int8_t io[4], int mode)
{
    if (h != 2 && h != 3) return -1;
    periph_enable(h == 2 ? PERIPH_SPI2 : PERIPH_SPI3);
    REG(CLK_GATE(h)) = BIT(0);
    REG(CLOCK(h)) = 0; REG(USER(h)) = 0; REG(CTRL(h)) = 0;
    REG(SLAVE(h)) = BIT(27);
    REG(SLAVE(h)) = BIT(26) | BIT(8) | BIT(9) | BIT(10) | BIT(11) |      /* slave, len from bit count (all 4 kinds) */
                    (mode == 1 || mode == 3 ? BIT(2) : 0);
    REG(USER(h)) = (mode == 1 || mode == 2 ? BIT(5) | BIT(8) : 0) | BIT(29) | BIT(30) | BIT(31);
    REG(USER1(h)) = 7u << 27 | 7;                                         /* 8 address bits, 8 dummy cycles */
    REG(USER2(h)) = 7u << 28;                                             /* 8 command bits */
    REG(MISC(h)) = mode >= 2 ? BIT(29) : 0;
    REG(DMA_CONF(h)) = FIFO_RST; REG(DMA_CONF(h)) = BIT(18);              /* segmented transfer mode */
    REG(INT_CLR(h)) = ~0u;
    gpio_config(sclk, GPIO_INPUT | GPIO_FORCE); gpio_in_signal(sclk, S(h).clk, false);
    gpio_config(cs, GPIO_INPUT | GPIO_FORCE);   gpio_in_signal(cs, S(h).cs, false);
    for (int i = 0; i < 4; i++) if (io[i] >= 0) {
        gpio_config(io[i], GPIO_OUTPUT | GPIO_INPUT);
        gpio_out_signal(io[i], S(h).io[i], false);
        gpio_in_signal(io[i], S(h).io[i], false);
    }
    return 0;
}

void spi_slave_hd_buf_write(int h, uint32_t addr, const void *src, size_t len)
{
    const uint8_t *b = src;
    for (size_t i = 0; i < len && addr + i < 64; i++) {
        uint32_t a = addr + i, sh = 8 * (a & 3);
        REG(W(h, a / 4)) = (REG(W(h, a / 4)) & ~(0xFFu << sh)) | (uint32_t)b[i] << sh;
    }
}

void spi_slave_hd_buf_read(int h, uint32_t addr, void *dst, size_t len)
{
    uint8_t *b = dst;
    for (size_t i = 0; i < len && addr + i < 64; i++) b[i] = REG(W(h, (addr + i) / 4)) >> (8 * ((addr + i) & 3));
}

uint32_t spi_slave_hd_events(int h)                      /* bit 11 master wrote, bit 10 master read; cleared */
{
    uint32_t r = REG(INT_RAW(h)) & (WR_BUF_DONE | RD_BUF_DONE);
    REG(INT_CLR(h)) = r;
    return r;
}

/* ---- slave half duplex, DMA (segment mode, IDF spi_slave_hd / essl_spi protocol): the master writes with
 * WRDMA (0x03) segments and ends with WR_END (0x07, slave event CMD7); it reads with RDDMA (0x04) segments and
 * ends with INT0 (0x08, CMD8). Arming follows IDF spi_slave_hd_hal_rxdma/txdma: FIFO, DMA half and slave reset. */
#define SLV_CMD7 BIT(4)
#define SLV_CMD8 BIT(5)
static gdma_desc_t hd_rxd[8], hd_txd[8];

static int hd_dma(int h)
{
    int k = h - 2;
    if (dma_ch[k] < 0) {
        if ((dma_ch[k] = gdma_alloc()) < 0) return -1;
        gdma_connect(dma_ch[k], h == 2 ? GDMA_SPI2 : GDMA_SPI3);
    }
    return dma_ch[k];
}

int spi_slave_hd_dma_rx(int h, void *buf, size_t len)
{
    int c = hd_dma(h);
    if (c < 0 || gdma_chain(hd_rxd, 8, buf, len, false) < 0) return -1;
    REG_SET(DMA_CONF(h), BIT(29)); REG_CLR(DMA_CONF(h), BIT(29));        /* rx afifo */
    gdma_in_reset(c);
    REG_SET(SLAVE(h), BIT(27)); REG_CLR(SLAVE(h), BIT(27));
    REG(INT_CLR(h)) = BIT(0) | SLV_CMD7;                                  /* infifo full, CMD7 */
    REG_SET(DMA_CONF(h), BIT(27));
    gdma_in_start(c, hd_rxd);
    return 0;
}

int spi_slave_hd_dma_rx_done(int h)
{
    if (!(REG(INT_RAW(h)) & SLV_CMD7)) return -1;
    int n = 0;
    for (gdma_desc_t *d = hd_rxd; d && !d->owner; d = d->next) n += d->length;   /* closed descriptors */
    return n;
}

int spi_slave_hd_dma_tx(int h, const void *buf, size_t len)
{
    int c = hd_dma(h);
    if (c < 0 || gdma_chain(hd_txd, 8, (void *)buf, len, true) < 0) return -1;
    REG_SET(DMA_CONF(h), BIT(31)); REG_CLR(DMA_CONF(h), BIT(31));        /* dma afifo */
    gdma_out_reset(c);
    REG_SET(SLAVE(h), BIT(27)); REG_CLR(SLAVE(h), BIT(27));
    REG(INT_CLR(h)) = BIT(1) | SLV_CMD8;                                  /* outfifo empty, CMD8 */
    REG_SET(DMA_CONF(h), BIT(28));
    gdma_out_start(c, hd_txd);
    return 0;
}

bool spi_slave_hd_dma_tx_done(int h) { return REG(INT_RAW(h)) & SLV_CMD8; }

void spi_dma_release(int h)                               /* give the host's GDMA channel back (5 in all) */
{
    int k = h - 2;
    if (dma_ch[k] >= 0) { REG(DMA_CONF(h)) = 0; gdma_free(dma_ch[k]); dma_ch[k] = -1; }
    dma_used[k] = false;
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
