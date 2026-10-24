#include "test.h"
#include "spi.h"
#include "gpio.h"
#include "sys.h"
#include <string.h>

/* No jumpers: one driver per pad, inputs cross-routed through the GPIO matrix.
 *  SPI2 master: CLK 14, CS 15, D0-D3 16 17 18 21 (D4-D7 38 39 40 41)
 *  SPI3 HD slave drives D0-D3 on 1 2 42 47; master reads those, slave reads the master's pads.
 * 1/2/4-line WRBUF + RDBUF (8-bit cmd, 8-bit addr, 8 dummy, IDF spi_slave_hd protocol), quad address,
 * then octal: master writes 16 bytes on 8 lines at 78 kHz, core 1 samples the pads on each rising SCLK. */
static const int8_t mpad[8] = { 16, 17, 18, 21, 38, 39, 40, 41 }, spad[4] = { 1, 2, 42, 47 };
static const uint8_t s2_io[4] = { 103, 102, 105, 104 }, s3_io[4] = { 68, 67, 70, 69 };

static bool hd(uint8_t mod, uint8_t lines, uint8_t alines, uint8_t addr, size_t n)
{
    uint8_t out[64], got[64];
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(i * 37 + lines * 11 + addr);
    spi_slave_hd_events(3);
    spi_hd_t w = { .cmd = SPI_HD_WRBUF | mod, .cmd_bits = 8, .addr = addr, .addr_bits = 8, .dummy = 8,
                   .lines = lines, .addr_lines = alines };
    spi_hd_xfer(2, &w, out, NULL, n);
    uint32_t ev = spi_slave_hd_events(3);
    memset(got, 0, sizeof got);
    spi_slave_hd_buf_read(3, addr, got, n);
    bool wok = (ev & BIT(11)) && !memcmp(out, got, n);
    for (size_t i = 0; i < n; i++) out[i] ^= 0xA5;
    spi_slave_hd_buf_write(3, addr, out, n);
    spi_hd_t r = w; r.cmd = SPI_HD_RDBUF | mod;
    memset(got, 0, sizeof got);
    spi_hd_xfer(2, &r, NULL, got, n);
    ev = spi_slave_hd_events(3);
    bool rok = (ev & BIT(10)) && !memcmp(out, got, n);
    printf("    %u-line data, %u-line addr, %2u B @ %u: write %s, read %s\n", lines, alines ? alines : 1, n, addr,
           wok ? "ok" : "BAD", rok ? "ok" : "BAD");
    if (!rok) { printf("     got"); for (size_t i = 0; i < 8; i++) printf(" %02x", got[i]); printf(" want"); for (size_t i = 0; i < 8; i++) printf(" %02x", out[i]); printf("\n"); }
    return wok && rok;
}

/* slave DMA (segment mode): WRDMA in 3 segments + WR_END lands back to back in the slave buffer; the slave's
 * buffer then goes back over RDDMA in 2 segments + INT0. */
extern uint8_t back[32768];                                /* test_fat.c: shared scratch */
#define dm back
#define ds (back + 16384)
static bool hd_dma(uint8_t mod, uint8_t lines)
{
    static const uint16_t wseg[3] = { 1000, 2000, 96 }, rseg[2] = { 2048, 1048 };
    for (int i = 0; i < 3096; i++) dm[i] = (uint8_t)(i * 13 + lines);
    memset(ds, 0, 3096);
    spi_hd_t w = { .cmd = SPI_HD_WRDMA | mod, .cmd_bits = 8, .addr_bits = 8, .dummy = 8, .lines = lines };
    bool ok = spi_slave_hd_dma_rx(3, ds, 3096) == 0;
    uint32_t t = ccount();
    for (int k = 0, o = 0; k < 3; o += wseg[k++]) spi_hd_xfer(2, &w, dm + o, NULL, wseg[k]);
    t = ccount() - t;
    bool before = spi_slave_hd_dma_rx_done(3) < 0;              /* not done until WR_END */
    w.cmd = SPI_HD_WR_END | mod;
    spi_hd_xfer(2, &w, NULL, NULL, 0);
    int n = spi_slave_hd_dma_rx_done(3);
    bool wok = ok && before && n == 3096 && !memcmp(dm, ds, 3096);
    for (int i = 0; i < 3096; i++) ds[i] = dm[i] ^ 0xA5;
    memset(dm, 0, 3096);
    spi_hd_t r = w; r.cmd = SPI_HD_RDDMA | mod;
    spi_slave_hd_dma_tx(3, ds, 3096);
    for (int k = 0, o = 0; k < 2; o += rseg[k++]) spi_hd_xfer(2, &r, NULL, dm + o, rseg[k]);
    bool rbefore = !spi_slave_hd_dma_tx_done(3);
    r.cmd = SPI_HD_INT0 | mod;
    spi_hd_xfer(2, &r, NULL, NULL, 0);
    bool rok = rbefore && spi_slave_hd_dma_tx_done(3) && !memcmp(dm, ds, 3096);
    printf("    DMA %u-line: WRDMA 3 seg + WR_END -> %d B %s (%lu us), RDDMA 2 seg + INT0 %s\n", lines, n,
           wok ? "ok" : "BAD", t / 240, rok ? "ok" : "BAD");
    return wok && rok;
}

static volatile uint32_t cap_go, cap_n, cap_both, cap_lo[64], cap_hi[64];
static void capture(void)                                /* core 1: sample GPIO_IN on each rising SCLK (GPIO14) */
{
    while (!cap_go) {}
    uint32_t prev = 0, t0 = ccount();
    while (cap_n < 64 && ccount() - t0 < 24000000u) {
        uint32_t lo = REG(0x6000403C), hi = REG(0x60004040);
        uint32_t ck = lo & BIT(14);
        if (ck != prev && (ck || cap_both)) { cap_lo[cap_n] = lo; cap_hi[cap_n] = hi; cap_n++; }
        prev = ck;
    }
    cap_go = 0;
    while (1) __asm__ volatile("waiti 0");
}

static bool octal(void)
{
    spi_master_init(2, 14, -1, -1, 15, 78125, 0);
    for (int i = 0; i < 8; i++) spi_master_io(2, i, mpad[i]);
    uint8_t out[16];
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(0x5A + i * 29);
    cap_n = 0; cap_both = 0; cap_go = 0;
    cpu1_start(capture);
    cap_go = 1;
    spi_hd_t t = { .lines = 8 };
    spi_hd_xfer(2, &t, out, NULL, sizeof out);
    uint32_t t0 = ccount(); while (cap_go && ccount() - t0 < 48000000u) {}
    int bad = 0;
    for (uint32_t k = 0; k < cap_n && k < 16; k++) {
        uint8_t b = 0;
        for (int i = 0; i < 8; i++) {
            int p = mpad[i];
            uint32_t v = p < 32 ? cap_lo[k] >> p : cap_hi[k] >> (p - 32);
            b |= (v & 1) << i;
        }
        if (b != out[k]) bad++;
    }
    printf("    octal write 16 B: %lu clocks captured, %d bytes wrong\n", cap_n, bad);
    return cap_n == 16 && bad == 0;
}

/* DTR: 1-line data on both SCLK edges at 78 kHz, core 1 samples D0 (GPIO16) at every edge: 8 bytes = 32 clocks */
static bool dtr(void)
{
    spi_master_init(2, 14, -1, -1, 15, 78125, 0);
    spi_master_io(2, 0, mpad[0]);
    uint8_t out[8] = { 0xA5, 0x3C, 0x0F, 0xF0, 0x81, 0x7E, 0x55, 0xC3 };
    cap_n = 0; cap_both = 1; cap_go = 0;
    cpu1_start(capture);
    cap_go = 1;
    spi_hd_t t = { .lines = 1, .dtr = SPI_DTR_DATA };
    spi_hd_xfer(2, &t, out, NULL, sizeof out);
    uint32_t t0 = ccount(); while (cap_go && ccount() - t0 < 48000000u) {}
    int bad = 0;
    for (int k = 0; k < 8; k++) {
        uint8_t b = 0;
        for (int i = 0; i < 8; i++) b = b << 1 | ((cap_lo[k * 8 + i] >> 16) & 1);
        bad += b != out[k];
    }
    printf("    DTR write 8 B on 1 line: %lu edges captured (64 = both edges), %d bytes wrong\n", cap_n, bad);
    spi_xfer(2, NULL, NULL, 0);
    return cap_n == 64 && bad == 0;
}

void test_spi_hd(void)
{
    CHECK(spi_slave_hd_init(3, 14, 15, spad, 0) == 0);           /* first: it makes 14/15 inputs */
    CHECK(spi_master_init(2, 14, -1, -1, 15, 10000000, 0) == 10000000);
    for (int i = 0; i < 4; i++) spi_master_io(2, i, mpad[i]);
    for (int i = 0; i < 4; i++) {                          /* cross the inputs */
        gpio_in_signal(spad[i], s2_io[i], false);
        gpio_in_signal(mpad[i], s3_io[i], false);
    }
    bool ok = hd(0, 1, 1, 0, 32);
    ok &= hd(SPI_HD_DUAL, 2, 1, 4, 32);
    ok &= hd(SPI_HD_QUAD, 4, 1, 8, 48);
    ok &= hd(SPI_HD_DUAL_ADDR, 2, 2, 0, 64);
    ok &= hd(SPI_HD_QUAD_ADDR, 4, 4, 16, 16);
    CHECK(ok);
    CHECK(hd_dma(0, 1));
    CHECK(hd_dma(SPI_HD_QUAD, 4));
    METRIC("spi_hd_xfer() quad write 32 B @10 MHz", "%lu cycles", ({ uint8_t b[32] = { 0 }; spi_hd_t w = { .cmd = SPI_HD_WRBUF | SPI_HD_QUAD, .cmd_bits = 8, .addr_bits = 8, .dummy = 8, .lines = 4 }; uint32_t t = ccount(); spi_hd_xfer(2, &w, b, NULL, 32); ccount() - t; }));
    CHECK(dtr());
    CHECK(octal());
    spi_dma_release(2); spi_dma_release(3);
}
