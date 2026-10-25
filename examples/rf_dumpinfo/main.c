/* Dump engine limits: capture rate per control-bit combination (time to
 * done for n words), playback position registers, config-register effects. */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include <stdint.h>
#include "../../drivers/rf/rf_pll.h"
#include "../../drivers/rf/rf_init.h"
#include "../../drivers/rf/rf_dump.h"

#define R(a) (*(volatile uint32_t *)(a))
#define CTRL 0x60033D5Cu
#define DAC  0x60033D64u
#define SENT 0xA5A0055Au

static uint32_t timed_capture(uint32_t bits, uint32_t n, uint32_t cfg, unsigned *written)
{
    volatile uint32_t *w = (volatile uint32_t *)RF_DUMP_BURST_ADDR;
    for (uint32_t k = 0; k < 16383; k++) w[k] = SENT;
    R(CTRL) = 0; R(0x60033D90) = cfg; R(0x600C101C) = 4;
    uint32_t ctrl = 1u << 31 | bits | n;
    R(CTRL) = ctrl;
    uint32_t t0 = ccount();
    R(CTRL) = ctrl | 1u << 19; R(CTRL) = ctrl;
    while (!(R(CTRL) & 1u << 18) && ccount() - t0 < 240000000u) {}
    uint32_t dt = ccount() - t0;
    R(CTRL) = 0; R(0x600C101C) = 0;
    unsigned c = 0; for (uint32_t k = 0; k < 16383; k++) c += w[k] != SENT;
    *written = c;
    return dt;
}

int main(void)
{
    delay_ms(1500);
    printf("=== rf_dumpinfo\n");
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2437000, &r);
    rf_rx_on();
    /* clock relations over 1 s of SYSTIMER (16 MHz, crystal): sample counter (circular write index,
     * polled and unwrapped), 0x60033D4C, CPU cycle counter */
    for (int rate = 0; rate < 2; rate++) {
        R(CTRL) = 0; R(0x60033D90) = 0x000C2040u; R(0x600C101C) = 1;
        R(CTRL) = 0x24000u | (rate ? 1u << 15 : 0); R(CTRL) = 0x24000u | (rate ? 1u << 15 : 0) | 1u << 31;
        uint32_t last = R(0x60033D60) & 0x3FFF, d4c0 = R(0x60033D4C);
        uint64_t samples = 0, s0 = time_ticks();
        uint32_t c0 = ccount();
        uint64_t s1;
        while ((s1 = time_ticks()) - s0 < 16000000u) {
            uint32_t w = R(0x60033D60) & 0x3FFF;
            samples += (w - last) & 0x3FFF; last = w;
        }
        uint32_t c1 = ccount(), d4c1 = R(0x60033D4C);
        R(CTRL) = 0; R(0x600C101C) = 0;
        double sec = (double)(s1 - s0) / 16e6;
        printf("rate %s: sample counter %.6f MS/s | D4C %.6f MHz | CPU %.6f MHz (over %.6f s of SYSTIMER)\n", rate ? "40" : "80",
               samples / sec / 1e6, (double)(uint32_t)(d4c1 - d4c0) / sec / 1e6, (double)(uint32_t)(c1 - c0) / sec / 1e6, sec);
    }
    printf("=== done\n");
    for (;;) {}
}
