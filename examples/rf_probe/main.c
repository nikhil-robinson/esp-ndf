/* RX DC calibration: residual DC per gain index with the vendor's codes
 * (replayed) vs our own calibration (rf_rx_dc_cal). */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include <stdint.h>
#include <math.h>
#include "../../drivers/rf/rf_pll.h"
#include "../../drivers/rf/rf_init.h"
#include "../../drivers/rf/rf_dump.h"

static void residual(const char *tag)
{
    float worst = 0, sum = 0; int n = 0;
    printf("%s:", tag);
    for (int g = 0; g < 128; g++) {
        rf_rx_gain((g + 64) & 127); rf_rx_gain(g); delay_us(50);
        const uint32_t ns = 4096;
        rf_dump_burst(RF_DUMP_80M, ns);
        const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
        float best = 1e30f, bi = 0, bq = 0;            /* quietest 512-sample block */
        for (uint32_t b = 0; b < ns; b += 512) {
            float si = 0, sq = 0, pp = 0;
            for (uint32_t k = b; k < b + 512; k++) { float i = rf_iq_i(w[k]), q = rf_iq_q(w[k]); si += i; sq += q; pp += i * i + q * q; }
            float var = pp / 512 - (si * si + sq * sq) / (512.0f * 512.0f);
            if (var < best) { best = var; bi = si / 512; bq = sq / 512; }
        }
        float m = fabsf(bi) > fabsf(bq) ? fabsf(bi) : fabsf(bq);
        if (m > worst) worst = m;
        sum += fabsf(bi) + fabsf(bq); n += 2;
        if (g % 8 == 0) printf(" g%d(%.1f,%.1f)", g, bi, bq);
    }
    printf("\n  worst |DC| %.2f LSB, mean |DC| %.2f LSB\n", worst, sum / n);
}

int main(void)
{
    delay_ms(1500);
    rf_init_replay();
    rf_pll_result_t r;
    rf_pll_set_khz(2437000, &r);
    rf_rx_on();
    residual("vendor codes (replayed)");
    uint32_t t0 = ccount();
    int w = rf_rx_dc_cal(128);
    uint32_t t1 = ccount();
    printf("rf_rx_dc_cal: %lu us, worst residual %.2f LSB\n", (unsigned long)((t1 - t0) / (sys_cpu_hz() / 1000000)), w / 16.0);
    residual("our calibration");
    for (int g = 0; g < 128; g += 20)
        printf("  row %3d I1 %3lu I2 %3lu Q1 %3lu Q2 %3lu\n", g, (unsigned long)(rf_gain_shadow[g][1] >> 22 & 0x1FF), (unsigned long)(rf_gain_shadow[g][1] >> 14 & 0xFF),
               (unsigned long)(rf_gain_shadow[g][0] >> 8 & 0x1FF), (unsigned long)(rf_gain_shadow[g][0] & 0xFF));
    printf("=== done\n");
    for (;;) {}
}
