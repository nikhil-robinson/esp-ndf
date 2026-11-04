/* Capture while the DAC plays: where does the capture data go? */
#include "console.h"
#include "cpu.h"
#include "sys.h"
#include <stdint.h>
#include <math.h>
#include "rf_pll.h"
#include "rf_init.h"
#include "rf_dump.h"

#undef REG
#define REG(a) (*(volatile uint32_t *)(a))
#define N 4000u

static void report(const char *tag)
{
    volatile uint32_t *b1 = (volatile uint32_t *)RF_DUMP_BANK(1), *b2 = (volatile uint32_t *)RF_DUMP_BANK(2);
    unsigned bad1 = 0, w2 = 0;
    for (unsigned k = 0; k < N; k++) {
        uint32_t want = rf_iq_word((int)lrintf(300 * cosf(2 * 3.14159265f * k / 20)), (int)lrintf(300 * sinf(2 * 3.14159265f * k / 20)));
        if (b1[k] != want) bad1++;
    }
    for (unsigned k = 0; k < 16383; k++) if (b2[k] != 0xA5A0055Au) w2++;
    printf("%-40s bank1 corrupted %4u/%u, bank2 written %5u, dac ctrl %08lx, bank sel %lx\n", tag, bad1, N, w2,
           (unsigned long)REG(0x60033D64), (unsigned long)REG(0x600C101C));
}

static void fill_tone(void)
{
    volatile uint32_t *b1 = (volatile uint32_t *)RF_DUMP_BANK(1);
    for (unsigned k = 0; k < N; k++)
        b1[k] = rf_iq_word((int)lrintf(300 * cosf(2 * 3.14159265f * k / 20)), (int)lrintf(300 * sinf(2 * 3.14159265f * k / 20)));
}

static void cap(uint32_t mask)
{
    volatile uint32_t *b2 = (volatile uint32_t *)RF_DUMP_BANK(2);
    for (unsigned k = 0; k < 16383; k++) b2[k] = 0xA5A0055Au;
    uint32_t sel = REG(0x600C101C), ctrl = 1u << 31 | 16383;
    REG(0x60033D5C) = 0; REG(0x60033D90) = 0x000C2040u;
    REG(0x600C101C) = (sel & ~15u) | mask;
    REG(0x60033D5C) = ctrl; REG(0x60033D5C) = ctrl | 1u << 19; REG(0x60033D5C) = ctrl;
    for (uint32_t t = 0; t < 1000000 && !(REG(0x60033D5C) & 1u << 18); t++) {}
    REG(0x60033D5C) = 0;
    REG(0x600C101C) = sel;
}

static void tone_check(const char *tag)
{
    const volatile uint32_t *b2 = (const volatile uint32_t *)RF_DUMP_BANK(2);
    unsigned live = 0; double si = 0, sq = 0, pp = 0;
    double re[2] = { 0 }, im[2] = { 0 };
    for (unsigned k = 0; k < N; k++) {
        uint32_t w = b2[k]; if (w >> 29 & 1) live++;
        double i = rf_iq_i(w), q = rf_iq_q(w), ph = 2 * 3.14159265358979 * k / 20;   /* 2 MHz at 40 MS/s */
        si += i; sq += q; pp += i * i + q * q;
        re[0] += i * cos(ph) + q * sin(ph); im[0] += q * cos(ph) - i * sin(ph);         /* +f */
        re[1] += i * cos(ph) - q * sin(ph); im[1] += q * cos(ph) + i * sin(ph);         /* -f */
    }
    double mi = si / N, mq = sq / N;
    printf("%-34s live %4u/%u mean %6.1f %6.1f rms %6.1f  |+2MHz| %7.2f |-2MHz| %7.2f\n", tag, live, N, mi, mq,
           sqrt(pp / N - mi * mi - mq * mq), sqrt(re[0] * re[0] + im[0] * im[0]) / N, sqrt(re[1] * re[1] + im[1] * im[1]) / N);
}

int main(void)
{
    delay_ms(1500);
    rf_init_replay();
    rf_pll_result_t r;
    rf_pll_set_khz(2437000, &r);
    rf_rx_on();
    fill_tone(); cap(4); report("A: no DAC, capture mask 4");
    rf_tx_on(127, 0);
    fill_tone(); rf_dac_play(1, N, 0, 1); delay_us(100); report("B0: DAC looping bank 1");
    cap(6); report("B: DAC + capture mask 6");
    rf_dac_stop(); fill_tone(); rf_dac_play(1, N, 0, 1);
    cap(4); report("C: DAC + capture mask 4 (DAC loses bank)");
    rf_dac_stop(); fill_tone(); rf_dac_play(1, N, 0, 1);
    cap(2); report("D: DAC + capture mask 2 (same bank)");
    rf_dac_stop(); rf_tx_off();
    fill_tone(); cap(4); report("E: after TX off, capture mask 4");
    /* estimator vs capture, RX */
    for (int g = 0; g <= 60; g += 20) {
        rf_rx_gain(g); delay_us(200);
        int32_t ei, eq, ep; rf_bb_est(2047, &ei, &eq, &ep);
        cap(4); char t[40]; snprintf(t, sizeof t, "rx gain %d est (%ld,%ld) p %ld", g, (long)ei, (long)eq, (long)ep);
        tone_check(t);
    }
    /* TX forced on + loopback: estimator vs TX row DC words */
    rf_tx_on(127, 0); rf_pbus_force_mode(1); rf_loopback(1, 10, 0x80); rf_rx_gain(20);
    unsigned base = REG(0x60006000) >> 18 & 0xFF;
    uint32_t lo0 = rf_gain_shadow[base][0], hi0 = rf_gain_shadow[base][1];
    for (int f = 0; f < 3; f++)
        for (int d = -64; d <= 64; d += 32) {
            uint32_t lo = lo0, hi = hi0;
            if (f == 0) lo = (lo & ~(0x1FFu << 12)) | ((((lo0 >> 12) & 0x1FF) + d) & 0x1FF) << 12;
            if (f == 1) lo = (lo & ~(0x1FFu << 3)) | ((((lo0 >> 3) & 0x1FF) + d) & 0x1FF) << 3;
            if (f == 2) hi = (hi & ~(0x1FFu << 17)) | ((((hi0 >> 17) & 0x1FF) + d) & 0x1FF) << 17;
            for (unsigned k = 0; k < 14; k++) rf_gain_mem_write(base + k, lo, hi);
            rf_txon(0); rf_txon(1); delay_us(100);
            int32_t ei, eq, ep; rf_bb_est(2047, &ei, &eq, &ep);
            printf("txon loopback field %d %+3d: est DC (%5ld,%5ld) power %ld\n", f, d, (long)ei, (long)eq, (long)ep);
        }
    for (unsigned k = 0; k < 14; k++) rf_gain_mem_write(base + k, lo0, hi0);
    printf("=== done\n");
    for (;;) {}
}
