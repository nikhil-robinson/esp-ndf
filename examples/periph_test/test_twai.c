#include "test.h"
#include "twai.h"
#include "gpio.h"

/* TX GPIO10 -> jumper -> RX GPIO11, self-test mode (no transceiver/ACK needed).
 * TX is mirrored onto GPIO4 (analyzer D1) for bit-time measurement. */
static int run(uint32_t rate)
{
    CHECK(twai_init(10, 11, rate, TWAI_SELFTEST) == 0);
    gpio_config(4, GPIO_OUTPUT); gpio_out_signal(4, 116, false);
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
    __asm__ volatile("rsil a2, 0" ::: "a2");
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
    METRIC("twai_send() 8 bytes @1M (wire incl.)", "%lu cycles", ({ twai_init(10, 11, 1000000, TWAI_SELFTEST); twai_frame_t f = { .id = 1, .dlc = 8 };
        uint32_t t = ccount(); twai_send(&f, 10000); ccount() - t; }));
}
