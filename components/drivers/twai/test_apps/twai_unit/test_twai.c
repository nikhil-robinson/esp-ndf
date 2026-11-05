#include "sys.h"
#include "test.h"
#include "twai.h"
#include "gpio.h"

#if SOC_CPU_RISCV
#define TWAI_TX_SIG 80
#else
#define TWAI_TX_SIG 116
#endif
/* TX GPIO10 -> jumper -> RX GPIO11, self-test mode (no transceiver/ACK needed).
 * TX is mirrored onto GPIO4 (analyzer D1) for bit-time measurement. */
static int RXP = 11;           /* the jumper's far end; TX pad itself when the board has none */
static int run(uint32_t rate)
{
    RXP = test_jumper(10, 11) ? 11 : 10;
    CHECK(twai_init(10, RXP, rate, TWAI_SELFTEST) == 0);
    gpio_config(4, GPIO_OUTPUT); gpio_out_signal(4, TWAI_TX_SIG, false);
    int ok = 0;
    for (int i = 0; i < 100; i++) {
        twai_frame_t f = { .id = i & 1 ? 0x1ABCDE00 + i : 0x100 + i, .ext = i & 1, .dlc = i % 9 }, g;
        for (int k = 0; k < 8; k++) f.data[k] = i * 8 + k;
        if (twai_send(&f, 10000) || twai_recv(&g, 10000)) continue;
        bool same = g.id == f.id && g.ext == f.ext && g.dlc == f.dlc;
        for (int k = 0; k < f.dlc; k++) same &= g.data[k] == f.data[k];
        ok += same;
    }
    uint8_t te, re; uint32_t dr;
    twai_counters(&te, &re, &dr);
    printf("    %4lu kbit/s: %d/100 frames round-trip identical (std+ext, dlc 0-8), tx err %d rx err %d dropped %lu\n",
           rate / 1000, ok, te, re, dr);
    return ok;
}

void test_twai(void)
{
    irq_enable_all();
    CHECK(run(125000) == 100);
    CHECK(run(1000000) == 100);
    CHECK(run(500000) == 100);
    printf("    analyzer: TWAI 500k\n");
    { twai_frame_t f = { .id = 0x555, .dlc = 8 }, g; for (int i = 0; i < 2000; i++) { twai_send(&f, 10000); twai_recv(&g, 10000); } }
    /* acceptance filter: only ID 0x123 */
    twai_filter(0x123, 0x7FF, false);
    twai_frame_t a = { .id = 0x123, .dlc = 1 }, b = { .id = 0x124, .dlc = 1 }, g;
    twai_send(&b, 10000); twai_send(&a, 10000);
    int r1 = twai_recv(&g, 5000), r2 = twai_recv(&g, 5000);
    printf("    filter 0x123: first recv id 0x%lx, second recv %s\n", g.id, r2 ? "none (0x124 rejected)" : "GOT ONE");
    CHECK(r1 == 0 && g.id == 0x123 && r2 == -1);
    METRIC("twai_send() 8 bytes @1M (wire incl.)", "%lu cycles", ({ twai_init(10, RXP, 1000000, TWAI_SELFTEST); twai_frame_t f = { .id = 1, .dlc = 8 };
        uint32_t t = ccount(); twai_send(&f, 10000); ccount() - t; }));
}

/* Error states without a second node: NORMAL mode on the jumper -> no ACK -> error warning, error passive;
 * RX held recessive -> every dominant bit is a bit error -> bus-off; RX back -> recovery -> RECOVERED. */
void test_twai_alerts(void)
{
    irq_enable_all();
    CHECK(twai_init(10, RXP, 500000, TWAI_NORMAL) == 0);
    twai_frame_t f = { .id = 0x321, .dlc = 2, .data = { 1, 2 } };
    twai_send(&f, 30000);                                    /* retransmits until aborted at the timeout */
    uint32_t a1 = twai_alerts(); uint8_t te; twai_counters(&te, 0, 0);
    int st1 = twai_state();
    printf("    no ACK: alerts %03lx, TEC %d, state %d (1 = passive)\n", a1, te, st1);
    CHECK((a1 & TWAI_ALERT_ABOVE_ERR_WARN) && (a1 & TWAI_ALERT_ERR_PASSIVE) && st1 == TWAI_ST_PASSIVE && te >= 128);

    gpio_in_signal(GPIO_IN_HIGH, TWAI_TX_SIG, false);                /* RX stuck recessive */
    twai_send(&f, 30000);
    uint32_t a2 = twai_alerts(); int st2 = twai_state();
    printf("    RX stuck recessive: alerts %03lx, state %d (2 = bus-off)\n", a2, st2);
    CHECK((a2 & TWAI_ALERT_BUS_OFF) && st2 == TWAI_ST_BUS_OFF);

    gpio_in_signal(RXP, TWAI_TX_SIG, false);
    uint32_t t0 = ccount();
    twai_recover();
    while (twai_state() != TWAI_ST_ACTIVE && ccount() - t0 < (sys_cpu_hz() / 1000) * 50) {}
    uint32_t us = (ccount() - t0) / (sys_cpu_hz() / 1000000), a3 = twai_alerts();
    printf("    recovery: alerts %03lx after %lu us (128 x 11 bits @500k = 2816 us)\n", a3, us);
    CHECK((a3 & TWAI_ALERT_RECOVERED) && us >= 2800 && us < 4000);
    twai_init(10, RXP, 500000, TWAI_SELFTEST);
    twai_frame_t g;
    CHECK(twai_send(&f, 10000) == 0 && twai_recv(&g, 10000) == 0 && g.id == 0x321);
}

NDF_UNIT(010, twai);
NDF_UNIT(020, twai_alerts);
