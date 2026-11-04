#include "test.h"
#include "rmt.h"
#include "sys.h"

/* TX on GPIO10 -> jumper wire -> RX on GPIO11 (docs/WIRING.md) */
#define TXP 10
#define RXP 11
#define N   100          /* > 48: exercises TX refill and RX drain */

static rmt_item_t out[N], in[N + 4];

void test_rmt(void)
{
    irq_enable_all();
    CHECK(rmt_tx_init(0, TXP, 10000000, 0) == 10000000);          /* 100 ns ticks */
    CHECK(rmt_rx_init(0, RXP, 10000000, 2000, 0) == 10000000);    /* idle 200 us ends frame */
    for (int i = 0; i < N; i++) out[i] = RMT_ITEM(1, 10 + i, 0, 20 + 2 * i);
    rmt_rx_start(0, in, N + 4);
    uint32_t t0 = ccount();
    CHECK(rmt_tx(0, out, N) == 0);
    uint32_t start_cyc = ccount() - t0;
    rmt_tx_wait(0);
    uint32_t tx_us = (ccount() - t0) / 240;
    int got = rmt_rx_wait(0, 100000);
    METRIC("TX 100 items / RX received", "%d items (+1 end marker)", got);
    CHECK(got >= N);
    /* durations must match within 1 tick (the RX samples at the same 100 ns) */
    int bad = 0, worst = 0;
    for (int i = 0; i < N && i < got; i++) {
        int e0 = (int)in[i].d0 - (int)out[i].d0, e1 = (int)in[i].d1 - (int)out[i].d1;
        if (i == N - 1) e1 = 0;                                     /* last low merges into idle */
        if (e0 < 0) e0 = -e0;
        if (e1 < 0) e1 = -e1;
        if (e0 > worst) worst = e0;
        if (e1 > worst) worst = e1;
        if (e0 > 1 || e1 > 1 || in[i].l0 != 1 || in[i].l1 != 0) bad++;
    }
    METRIC("worst duration error", "%d ticks (100 ns)", worst);
    CHECK(bad == 0);
    uint32_t expect_us = 0;
    for (int i = 0; i < N; i++) expect_us += out[i].d0 + out[i].d1;
    METRIC("TX time / signal length", "%lu / %lu us", tx_us, expect_us / 10);
    METRIC("rmt_tx() start (100 items)", "%lu cycles", start_cyc);
}

NDF_UNIT(010, rmt);
