/* TX gain-row field sweep, measured over the air. Board ...D0 transmits a
 * +2 MHz tone and steps through field offsets (600 ms each, with a 600 ms
 * "off" marker between fields); the other board logs tone / image / DC. */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include <stdint.h>
#include <math.h>
#include "../../drivers/rf/rf_pll.h"
#include "../../drivers/rf/rf_init.h"
#include "../../drivers/rf/rf_dump.h"

#undef REG
#define REG(a) (*(volatile uint32_t *)(a))

static float bin_db(uint32_t n, float f)
{
    const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
    float c = cosf(2 * (float)M_PI * f / 80e6f), s = sinf(2 * (float)M_PI * f / 80e6f), re = 0, im = 0, cr = 1, ci = 0;
    for (uint32_t k = 0; k < n; k++) {
        float i = rf_iq_i(w[k]), q = -rf_iq_q(w[k]);
        re += i * cr + q * ci; im += q * cr - i * ci;
        float t = cr * c - ci * s; ci = ci * c + cr * s; cr = t;
        if ((k & 255) == 255) { float m = 1.0f / sqrtf(cr * cr + ci * ci); cr *= m; ci *= m; }
    }
    return 10 * log10f((re * re + im * im) / ((float)n * n) + 1e-6f);
}

/* field f of a TX row word pair: 0 w0 lo[20:12], 1 w1 lo[11:3], 2 w3 hi[25:17], 3 cal2 lo[31:21] (11b), 4 w2 hi[31:26]+lo[2:0] */
static void field_add(uint32_t *lo, uint32_t *hi, int f, int d)
{
    if (f == 0) *lo = (*lo & ~(0x1FFu << 12)) | (((*lo >> 12) + d) & 0x1FF) << 12;
    if (f == 1) *lo = (*lo & ~(0x1FFu << 3)) | (((*lo >> 3) + d) & 0x1FF) << 3;
    if (f == 2) *hi = (*hi & ~(0x1FFu << 17)) | (((*hi >> 17) + d) & 0x1FF) << 17;
    if (f == 3) *lo = (*lo & ~(0x7FFu << 21)) | (((*lo >> 21) + d) & 0x7FF) << 21;
    if (f == 4) { uint32_t w2 = (*hi >> 26) | (*lo & 7) << 6; w2 = (w2 + d) & 0x1FF; *hi = (*hi & 0x03FFFFFFu) | (w2 & 63) << 26; *lo = (*lo & ~7u) | w2 >> 6; }
}

int main(void)
{
    delay_ms(1500);
    uint32_t mac0 = REG(0x60007044);
    int tx = (mac0 & 0xFF) == 0xD0 || (mac0 >> 24) == 0xD0;
    printf("=== rf_txsweep role %s\n", tx ? "TX" : "RX");
    rf_init_replay();
    rf_pll_result_t r;
    rf_pll_set_khz(2437000, &r);
    rf_rx_on();
    rf_rx_dc_cal(128);
    if (tx) {
        const uint32_t n = 2000;
        volatile uint32_t *b = (volatile uint32_t *)RF_DUMP_BANK(1);
        for (uint32_t k = 0; k < n; k++)
            b[k] = rf_iq_word((int)lrintf(300 * cosf(2 * (float)M_PI * k / 20)), (int)lrintf(300 * sinf(2 * (float)M_PI * k / 20)));
        delay_ms(2000);                          /* receiver ready */
        rf_tx_on(127, 0);
        rf_dac_play(1, n, 0, 1);
        unsigned base = REG(0x60006000) >> 18 & 0xFF;
        uint32_t lo0 = rf_gain_shadow[base][0], hi0 = rf_gain_shadow[base][1];
        static const int d[] = { 0, -32, -16, 16, 32, 0 };
        for (int f = 0; f < 5; f++) {
            for (unsigned k = 0; k < 14; k++) rf_gain_mem_write(base + k, lo0, hi0);
            rf_txon(0); delay_ms(600);            /* marker: off */
            for (unsigned s = 0; s < sizeof d / sizeof d[0]; s++) {
                uint32_t lo = lo0, hi = hi0;
                field_add(&lo, &hi, f, d[s]);
                for (unsigned k = 0; k < 14; k++) rf_gain_mem_write(base + k, lo, hi);
                rf_txon(0); rf_txon(1);
                printf("field %d %+d\n", f, d[s]);
                delay_ms(600);
            }
        }
        rf_dac_stop(); rf_tx_off();
    } else {
        uint64_t t0 = time_us();
        while (time_us() - t0 < 30000000) {
            rf_rx_gain(20); delay_us(200);
            rf_dump_burst(RF_DUMP_80M, 8192);
            printf("t %6lu tone %6.1f image %6.1f dc %6.1f\n", (unsigned long)((time_us() - t0) / 1000),
                   bin_db(8192, 2e6f), bin_db(8192, -2e6f), bin_db(8192, 0));
            delay_ms(100);
        }
    }
    printf("=== done\n");
    for (;;) {}
}
