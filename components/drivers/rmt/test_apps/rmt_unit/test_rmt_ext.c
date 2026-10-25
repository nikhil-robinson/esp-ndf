#include "test.h"
#include "rmt.h"
#include "gpio.h"
#include "sys.h"
#include <string.h>

/* TX 10 -> jumper -> RX 11; TX ch1 also on pad 14 for the sync skew (read back through GPIO_IN). */
#define GPIO_IN 0x6000403C
static rmt_item_t out[1024], in[1100];

static int edges(const rmt_item_t *b, int n)               /* rising edges in received items */
{
    int k = 0;
    for (int i = 0; i < n; i++) k += b[i].l0 == 1 && b[i].d0;
    return k;
}

static void loop(void)
{
    rmt_tx_init(0, 10, 1000000, 0);                         /* 1 us ticks */
    rmt_rx_init(0, 11, 1000000, 500, 0);
    rmt_item_t p[3] = { RMT_ITEM(1, 5, 0, 5), RMT_ITEM(1, 10, 0, 5), RMT_ITEM(1, 3, 0, 7) };
    rmt_rx_start(0, in, 1100);
    CHECK(rmt_tx_loop(0, p, 3, 100) == 0);
    rmt_tx_wait(0);
    int n = rmt_rx_wait(0, 100000);
    printf("    loop 3 items x 100: TX done, RX %d items, %d pulses (300)\n", n, edges(in, n));
    CHECK(edges(in, n) == 300);
}

static uint32_t skew(bool sync)
{
    rmt_tx_init(0, 12, 1000000, 0);
    rmt_tx_init(1, 14, 1000000, 0);
    static const rmt_item_t one[1] = { RMT_ITEM(1, 20, 0, 5) };
    rmt_tx_sync(sync ? 3 : 0);
    uint32_t t1 = 0, t2 = 0, t0 = ccount(), prev = ~0u;
    rmt_tx(0, one, 1);
    for (volatile int d = 0; d < 200; d++) {}               /* the second start comes ~1 us later */
    rmt_tx(1, one, 1);
    /* time the falling edges: the 20 us pulses outlast any start-up latency of this loop (flash cache) */
    while ((!t1 || !t2) && ccount() - t0 < 2400000u) {
        uint32_t v = REG(GPIO_IN), now = ccount();
        if (!t1 && (prev & BIT(12)) && !(v & BIT(12))) t1 = now;
        if (!t2 && (prev & BIT(14)) && !(v & BIT(14))) t2 = now;
        prev = v;
    }
    rmt_tx_wait(0); rmt_tx_wait(1);
    rmt_tx_sync(0);
    return t1 > t2 ? t1 - t2 : t2 - t1;
}

static void demod(void)
{
    rmt_tx_init(0, 10, 1000000, 0);
    rmt_tx_carrier(0, 38000, 33);
    for (int i = 0; i < 5; i++) out[i] = RMT_ITEM(1, 600, 0, 600);
    rmt_rx_init(0, 11, 10000000, 20000, 0);                /* 100 ns: the carrier is 263 ticks */
    rmt_rx_start(0, in, 1100);
    rmt_tx(0, out, 5); rmt_tx_wait(0);
    int raw = rmt_rx_wait(0, 100000);
    rmt_rx_demod(0, 38000, 10000000, 33, true);
    rmt_rx_start(0, in, 1100);
    rmt_tx(0, out, 5); rmt_tx_wait(0);
    int n = rmt_rx_wait(0, 100000);
    int hi = n > 1 ? in[1].d0 / 10 : 0;
    printf("    38 kHz bursts 5 x 600 us: raw %d items, demodulated %d items, high %d us\n", raw, n, hi);
    CHECK(raw > 50 && n >= 5 && n <= 6 && hi > 560 && hi < 640);
    rmt_rx_demod(0, 0, 0, 0, false);
    rmt_tx_carrier(0, 0, 0);
}

static void dma(void)
{
    for (int i = 0; i < 1000; i++) out[i] = RMT_ITEM(1, 5 + i % 7, 0, 4 + i % 5);
    rmt_tx_init(3, 10, 10000000, 0);                        /* 100 ns ticks */
    rmt_rx_init(0, 11, 10000000, 300, 0);
    rmt_rx_start(0, in, 1100);
    CHECK(rmt_tx_dma(3, out, 1000) == 0);
    { uint32_t t0 = ccount(); while (ccount() - t0 < 24000000u) {} }        /* 1000 items ~ 1 ms */
    int n = rmt_rx_wait(0, 100000), bad = 0;
    for (int i = 0; i < 999 && i < n; i++)
        bad += in[i].d0 < out[i].d0 - 1 || in[i].d0 > out[i].d0 + 1 || in[i].d1 < out[i].d1 - 1 || in[i].d1 > out[i].d1 + 1;
    printf("    TX DMA (ch 3) 1000 items -> RX: %d items, %d off by > 1 tick\n", n, bad);
    CHECK(n >= 1000 && bad == 0);
    /* RX DMA (ch 3) from a normal TX */
    rmt_tx_init(0, 10, 10000000, 0);
    rmt_rx_init(3, 11, 10000000, 300, 0);
    memset(in, 0, sizeof in);
    CHECK(rmt_rx_dma_start(3, in, 1100) == 0);
    rmt_tx(0, out, 1000); rmt_tx_wait(0);
    n = rmt_rx_dma_wait(3, 100000); bad = 0;
    for (int i = 0; i < 999 && i < n; i++)
        bad += in[i].d0 < out[i].d0 - 1 || in[i].d0 > out[i].d0 + 1 || in[i].d1 < out[i].d1 - 1 || in[i].d1 > out[i].d1 + 1;
    printf("    TX -> RX DMA (ch 3): %d items, %d off by > 1 tick\n", n, bad);
    CHECK(n >= 1000 && bad == 0);
}

static void ws2812(void)
{
    const uint8_t grb[9] = { 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0xA5, 0x5A, 0xFF };
    size_t n = rmt_ws2812_encode(grb, 9, out, 10000000);
    rmt_tx_init(0, 10, 10000000, 0);
    rmt_rx_init(0, 11, 10000000, 500, 0);
    rmt_rx_start(0, in, 1100);
    rmt_tx(0, out, n); rmt_tx_wait(0);
    int m = rmt_rx_wait(0, 100000);
    uint8_t dec[9] = { 0 };
    for (int i = 0; i < 72 && i < m; i++) dec[i / 8] |= (in[i].d0 > 6) << (7 - i % 8);   /* 0.8 us = 1 */
    printf("    WS2812 3 LEDs: %u items, decoded %s (T1H %u, T0H %u ticks)\n", n, memcmp(dec, grb, 9) ? "BAD" : "ok",
           in[8 * 8].d0, in[0].d0);
    CHECK(n == 72 && !memcmp(dec, grb, 9));
    uint32_t bc = ~0u, bd = ~0u;
    for (int k = 0; k < 8; k++) {
        uint32_t c = ccount();
        rmt_tx(0, out, rmt_ws2812_encode(grb, 9, out, 10000000));
        uint32_t c1 = ccount();
        rmt_tx_wait(0);
        uint32_t c2 = ccount();
        if (c1 - c < bc) bc = c1 - c;
        if (c2 - c < bd) bd = c2 - c;
        delay_us(100);
    }
    METRIC("WS2812 3 LEDs encode + rmt_tx()", "%lu cycles", bc);
    METRIC("WS2812 3 LEDs until done", "%lu cycles (wire 21600)", bd);
}

void test_rmt_ext(void)
{
    __asm__ volatile("rsil a2, 0" ::: "a2");
    loop();
    uint32_t s0 = skew(false), s1 = skew(true);
    printf("    start skew ch0/ch1: %lu cycles without sync, %lu with sync\n", s0, s1);
    CHECK(s1 < 100 && s0 > s1);
    demod();
    dma();
    ws2812();
}

NDF_UNIT(020, rmt_ext);
