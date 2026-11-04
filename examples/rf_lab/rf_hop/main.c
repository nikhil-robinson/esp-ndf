/* Retune speed: full rf_pll_set_khz vs rf_pll_hop with a cached cap, and the
 * LO settling seen over the air. Board ...D0 holds a 2439 MHz tone (LO 2437 +
 * 2 MHz DAC tone); the other parks at 2500 MHz, hops to 2436 MHz, captures at
 * once and reports the tone (+3 MHz) per 256-sample block (3.2 us). */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include <stdint.h>
#include <math.h>
#include "rf_pll.h"
#include "rf_regi2c.h"
#include "rf_init.h"
#include "rf_dump.h"

static float blk_db(const uint32_t *w, uint32_t n, float f)
{
    float c = cosf(2 * (float)M_PI * f / 80e6f), s = sinf(2 * (float)M_PI * f / 80e6f), re = 0, im = 0, cr = 1, ci = 0;
    for (uint32_t k = 0; k < n; k++) {
        float i = rf_iq_i(w[k]), q = -rf_iq_q(w[k]);
        re += i * cr + q * ci; im += q * cr - i * ci;
        float t = cr * c - ci * s; ci = ci * c + cr * s; cr = t;
    }
    return 10 * log10f((re * re + im * im) / ((float)n * n) + 1e-6f);
}

int main(void)
{
    delay_ms(1500);
    uint32_t mac0 = *(volatile uint32_t *)0x60007044;
    int tx = (mac0 & 0xFF) == 0xD0 || (mac0 >> 24) == 0xD0;
    printf("=== rf_hop role %s\n", tx ? "TX" : "RX");
    rf_init_replay();
    rf_pll_result_t r;
    rf_rx_on();
    if (tx) {
        rf_pll_set_khz(2437000, &r);
        const uint32_t n = 2000;
        volatile uint32_t *b = (volatile uint32_t *)RF_DUMP_BANK(1);
        for (uint32_t k = 0; k < n; k++)
            b[k] = rf_iq_word((int)lrintf(300 * cosf(2 * (float)M_PI * k / 20)), (int)lrintf(300 * sinf(2 * (float)M_PI * k / 20)));
        rf_tx_on(127, 0); rf_dac_play(1, n, 0, 1);
        delay_ms(20000);
        rf_dac_stop(); rf_tx_off();
    } else {
        rf_rx_dc_cal(128);
        uint32_t mhz = sys_cpu_hz() / 1000000;
        rf_pll_result_t a, b;
        rf_pll_set_khz(2500000, &a);
        uint32_t t0 = ccount(); rf_pll_set_khz(2436000, &b); uint32_t full = ccount() - t0;
        printf("full retune %lu us (cap %03x), park cap %03x\n", (unsigned long)((full) / mhz), b.cap, a.cap);
        rf_rx_gain(20);
        delay_ms(2000);                           /* transmitter on */
        rf_pll_set_khz(2436000, &b);
        rf_dump_burst(RF_DUMP_80M, 8192);
        printf("control (full tune): tone %.1f dB\n", blk_db((const uint32_t *)RF_DUMP_BURST_ADDR, 8192, 3e6f));
        for (int rep = 0; rep < 3; rep++) {
            rf_pll_hop(2500000, a.cap);
            delay_us(500);
            t0 = ccount();
            rf_pll_hop(2436000, b.cap);
            uint32_t hop = ccount() - t0;
            rf_dump_burst(RF_DUMP_80M, 8192);       /* starts a few us after the hop */
            const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
            printf("hop %lu cycles = %.1f us; tone per 3.2 us block:", (unsigned long)hop, (float)hop / mhz);
            for (int k = 0; k < 24; k++) printf(" %.0f", blk_db(w + 256 * k, 256, 3e6f));
            printf("  | late: %.1f dB\n", blk_db(w + 6144, 2048, 3e6f));
        }
        /* bus cost of one analog write */
        t0 = ccount(); for (int k = 0; k < 100; k++) rf_regi2c_write(0x63, 0x05, (uint8_t)k); uint32_t wc = (ccount() - t0) / 100;
        printf("analog-bus write %lu cycles = %.2f us\n", (unsigned long)wc, (float)wc / mhz);
        rf_pll_hop(2436000, b.cap);
    }
    printf("=== done\n");
    for (;;) {}
}
