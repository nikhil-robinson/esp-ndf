/* Over-the-air check of our transmit path, two boards at 2437 MHz.
 * Board ...D0 transmits a +2 MHz DAC tone, 1 s on / 1 s off; the other
 * receives and reports the power at +2 / -2 / +5 MHz (control) per capture. */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include <stdint.h>
#include <math.h>
#include "../../drivers/rf/rf_pll.h"
#include "../../drivers/rf/rf_init.h"
#include "../../drivers/rf/rf_dump.h"

static float bin_db(uint32_t n, float f)        /* dBFS-ish power of one bin, 80 MS/s */
{
    const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
    float c = cosf(2 * (float)M_PI * f / 80e6f), s = sinf(2 * (float)M_PI * f / 80e6f), re = 0, im = 0, cr = 1, ci = 0;
    for (uint32_t k = 0; k < n; k++) {
        float i = rf_iq_i(w[k]), q = rf_iq_q(w[k]);
        re += i * cr + q * ci; im += q * cr - i * ci;
        float t = cr * c - ci * s; ci = ci * c + cr * s; cr = t;
        if ((k & 255) == 255) { float m = 1.0f / sqrtf(cr * cr + ci * ci); cr *= m; ci *= m; }
    }
    return 10 * log10f((re * re + im * im) / ((float)n * n) + 1e-6f);
}

int main(void)
{
    delay_ms(1500);
    uint32_t mac0 = *(volatile uint32_t *)0x60007044;
    int tx = (mac0 & 0xFF) == 0xD0 || (mac0 >> 24) == 0xD0;
    printf("=== rf_ota mac0 %08lx role %s\n", (unsigned long)mac0, tx ? "TX" : "RX");
    rf_init_replay();
#ifndef RX_KHZ
#define RX_KHZ OTA_KHZ
#endif
#ifndef OTA_KHZ
#define OTA_KHZ 2437000
#endif
    rf_rx_on();
    int trc;
    if (tx) { rf_pll_result_t r; trc = rf_pll_set_khz(OTA_KHZ, &r); if (!trc && !r.locked) trc = -3; }
    else trc = rf_tune_rx_khz(RX_KHZ);
    printf("tune rc %d\n", trc);
    rf_rx_dc_cal(128);
    if (tx) {
        const uint32_t n = 2000;
        volatile uint32_t *b = (volatile uint32_t *)RF_DUMP_BANK(1);
        for (uint32_t k = 0; k < n; k++)
            b[k] = rf_iq_word((int)lrintf(300 * cosf(2 * (float)M_PI * k / 20)), (int)lrintf(300 * sinf(2 * (float)M_PI * k / 20)));
        for (int s = 0; s < 12; s++) {
            if (s & 1) { rf_dac_stop(); rf_tx_off(); printf("tx off\n"); }
            else { rf_tx_on(127, 0); rf_dac_play(1, n, 0, 1); printf("tx on\n"); }
            delay_ms(1000);
        }
        rf_dac_stop(); rf_tx_off();
    } else {
        for (int k = 0; k < 48; k++) {
            rf_rx_gain(20);
            delay_us(200);
            rf_dump_burst(RF_DUMP_80M, 8192);
            printf("t %5lu ms  +2MHz %6.1f dB  -2MHz %6.1f dB  +5MHz %6.1f dB\n", (unsigned long)time_ms(),
                   bin_db(8192, 2e6f), bin_db(8192, -2e6f), bin_db(8192, 5e6f));
            delay_ms(250);
        }
    }
    printf("=== done\n");
    for (;;) {}
}
