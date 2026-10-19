/* Blob-free radio on esp-ndf: our PHY init (replay), PLL, receive with the
 * dump engine at forced gains, then DAC playback + capture together. Same
 * output format as bench/idf_rfrx (vendor and ours on IDF, same board). */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "../../drivers/rf/rf_regi2c.h"
#include "../../drivers/rf/rf_pll.h"
#include "../../drivers/rf/rf_init.h"
#include "../../drivers/rf/rf_dump.h"

static void capture(int gain)
{
    rf_rx_gain(gain);
    delay_ms(2);
    const uint32_t n = RF_DUMP_MAX;
    int rc = rf_dump_burst(RF_DUMP_80M, n);
    const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
    double si = 0, sq = 0, si2 = 0, sq2 = 0; int pk = 0;
    for (uint32_t k = 0; k < n; k++) {
        int i = rf_iq_i(w[k]), q = rf_iq_q(w[k]);
        si += i; sq += q; si2 += (double)i * i; sq2 += (double)q * q;
        if (abs(i) > pk) pk = abs(i);
        if (abs(q) > pk) pk = abs(q);
    }
    double mi = si / n, mq = sq / n;
    printf("gain %3d rc=%d dc=(%6.1f,%6.1f) rms=(%6.1f,%6.1f) peak=%d first=%08lx\n", gain, rc, mi, mq,
           sqrt(si2 / n - mi * mi), sqrt(sq2 / n - mq * mq), pk, (unsigned long)w[0]);
}

static double bin_power(uint32_t n, double f)
{
    const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
    double c = cos(2 * M_PI * f / 80e6), s = sin(2 * M_PI * f / 80e6), re = 0, im = 0, cr = 1, ci = 0;
    for (uint32_t k = 0; k < n; k++) {
        double i = rf_iq_i(w[k]), q = rf_iq_q(w[k]);
        re += i * cr + q * ci; im += q * cr - i * ci;
        double t = cr * c - ci * s; ci = ci * c + cr * s; cr = t;
    }
    return (re * re + im * im) / ((double)n * n);
}

static void tx_test(void)
{
    const uint32_t n = 2000;
    volatile uint32_t *b = (volatile uint32_t *)RF_DUMP_BANK(1);
    for (uint32_t k = 0; k < n; k++)
        b[k] = rf_iq_word((int)lrint(300 * cos(2 * M_PI * k / 20)), (int)lrint(300 * sin(2 * M_PI * k / 20)));
    for (int on = 0; on < 2; on++) {
        if (on) {
            rf_tx_on(127, 0);
            rf_dac_play(1, n, 0, 1);
        }
        for (int g = 0; g <= 60; g += 20) {
            rf_rx_gain(g);
            delay_us(200);
            rf_dump_burst(RF_DUMP_80M, RF_DUMP_MAX);
            printf("tx %s gain %2d: +2MHz %10.1f  -2MHz %10.1f  +5MHz %8.1f\n", on ? "on " : "off", g,
                   bin_power(RF_DUMP_MAX, 2e6), bin_power(RF_DUMP_MAX, -2e6), bin_power(RF_DUMP_MAX, 5e6));
        }
    }
    rf_dac_stop();
    rf_tx_off();
}

static uint32_t cb_cycles;
static int32_t sum_i(const uint32_t *a, uint32_t n)
{
    int32_t s0 = 0, s1 = 0;
    uint32_t k = 0;
    for (; k + 2 <= n; k += 2) { s0 += (int32_t)(a[k] << 22) >> 22; s1 += (int32_t)(a[k + 1] << 22) >> 22; }
    if (k < n) s0 += rf_iq_i(a[k]);
    return s0 + s1;
}
static int sum_cb(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, void *ctx)
{
    uint32_t t = ccount();
    *(int64_t *)ctx += sum_i(a, na) + sum_i(b, nb);
    t = ccount() - t;
    if (t > cb_cycles) cb_cycles = t;
    return 0;
}

static int light_cb(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, void *ctx)
{
    (void)b; (void)nb;
    *(int64_t *)ctx += rf_iq_i(a[0]) + rf_iq_i(a[na / 2]);
    return 0;
}

static void stream_test(void)
{
    {
        rf_stream_stats_t st; int64_t acc = 0;
        uint32_t ps = irq_lock();
        uint64_t t0 = time_us();
        int rc = rf_stream(RF_DUMP_80M, 1000, light_cb, &acc, &st);
        uint64_t t1 = time_us();
        irq_unlock(ps);
        printf("stream 80 MS/s, light consumer: rc=%d units=%lu pairs=%llu late_max=%lu  %.3f MS/s measured\n", rc,
               (unsigned long)st.units, (unsigned long long)st.pairs, (unsigned long)st.late_max, (double)st.pairs / (double)(t1 - t0));
    }
    static const struct { uint32_t rate; const char *name; double hz; } r[] = {
        { RF_DUMP_40M, "40", 40e6 }, { RF_DUMP_16M, "16", 16e6 } };   /* full-sample consumer: too slow for 80 on one core */
    for (unsigned k = 0; k < 2; k++) {
        rf_stream_stats_t st; int64_t acc = 0;
        uint32_t ps = irq_lock();
        uint64_t t0 = time_us();
        int rc = rf_stream(r[k].rate, 300, sum_cb, &acc, &st);
        uint64_t t1 = time_us();
        irq_unlock(ps);
        printf("  callback max %lu cycles (%.2f per pair)\n", (unsigned long)cb_cycles, cb_cycles / 12288.0);
        cb_cycles = 0;
        printf("stream %s MS/s: rc=%d units=%lu pairs=%llu late_max=%lu  %.3f MS/s measured, mean I %.2f\n", r[k].name, rc,
               (unsigned long)st.units, (unsigned long long)st.pairs, (unsigned long)st.late_max,
               (double)st.pairs / (double)(t1 - t0), st.pairs ? (double)acc / (double)st.pairs : 0.0);
    }
}

int main(void)
{
    delay_ms(1500);
    printf("=== esp-ndf (no vendor code)\n");
    uint32_t t0 = ccount();
    rf_init_replay();
    uint32_t t1 = ccount();
    rf_pll_result_t r;
    int rc = rf_pll_set_khz(2437000, &r);
    uint32_t t2 = ccount();
    rf_rx_on();
    uint32_t t3 = ccount();
    uint32_t mhz = sys_cpu_hz() / 1000000;
    printf("init %lu us, pll %lu us (rc=%d cap=%03x locked=%d), rx on %lu us\n", (unsigned long)((t1 - t0) / mhz),
           (unsigned long)((t2 - t1) / mhz), rc, r.cap, r.locked, (unsigned long)((t3 - t2) / mhz));
    static const int gains[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, -1 };
    for (unsigned g = 0; g < sizeof gains / sizeof gains[0]; g++) capture(gains[g]);
    stream_test();
    tx_test();
    printf("=== done\n");
    for (;;) {}
}
