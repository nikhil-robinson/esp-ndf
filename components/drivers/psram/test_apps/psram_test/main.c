/* PSRAM + RF data path: gapless capture into PSRAM (rf_rx_capture_to) and triggered capture (rf_rx_capture_trigger).
 * D0 (rf_cal app) sends a tone at 2440 MHz + 1 MHz; C9:48 captures at LO 2440 MHz. */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "psram.h"
#include "rf.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
static double tone_db8(const uint16_t *s, uint32_t n, float f, float fs)   /* IQ8 tone power, dB */
{
    float w = 2 * 3.14159265f * f / fs, c = cosf(w), sn = sinf(w), re = 1, im = 0, ai = 0, aq = 0;
    for (uint32_t k = 0; k < n; k++) { float i = (int8_t)(s[k] & 0xFF), q = -(int8_t)(s[k] >> 8); ai += i * re + q * im; aq += q * re - i * im; float t = re * c - im * sn; im = re * sn + im * c; re = t; }
    return 10 * log10(((double)ai * ai + (double)aq * aq) / ((double)n * n) + 1e-12);
}
int main(void)
{
    delay_ms(300);
    uint32_t sz = psram_init(2);
    printf("psram %lu\n", (unsigned long)sz);
    rf_open(); rf_set_freq_khz(2440000); rf_rx_start(); int db; rf_rx_autogain(100, &db);
    uint16_t *p = (uint16_t *)PSRAM_VADDR; uint32_t n = sz / 2;
    memset(p, 0, 4096); psram_writeback(p, 4096);
    uint32_t t0 = ccount(); int rc = rf_rx_capture_to(RF_RATE_16M, RF_FMT_IQ8, n, p); uint32_t t1 = ccount();
    psram_invalidate(p, sz);
    printf("capture 16 MS/s IQ8: rc %d, %lu samples (%.1f ms of air) in %.1f ms | tone at start %.1f mid %.1f end %.1f dB, off-tone %.1f dB\n", rc, (unsigned long)n,
           n / 16e3, (t1 - t0) / 240e3, tone_db8(p, 4096, 1e6f, 16e6f), tone_db8(p + n / 2, 4096, 1e6f, 16e6f), tone_db8(p + n - 4096, 4096, 1e6f, 16e6f), tone_db8(p + n / 2, 4096, 3e6f, 16e6f));
    uint32_t t2 = ccount(); rc = rf_rx_capture_to(RF_RATE_16M, RF_FMT_IQ32, sz / 4, (void *)PSRAM_VADDR); uint32_t t3 = ccount();
    extern int rf_cap_last_rc; extern uint32_t rf_cap_last_late, rf_cap_last_units;
    printf("   (rf_stream rc %d units %lu late_max %lu)\n", rf_cap_last_rc, (unsigned long)rf_cap_last_units, (unsigned long)rf_cap_last_late);
    printf("capture 16 MS/s IQ32 into PSRAM: rc %d (%.1f ms)\n", rc, (t3 - t2) / 240e3);
    rc = rf_rx_capture_to(RF_RATE_40M, RF_FMT_IQ8, sz / 2, p);
    printf("capture 40 MS/s IQ8 into PSRAM: rc %d\n", rc);
    printf("ready for trigger\n");
    while (console_getc() != 'g') {}                       /* host: transmitter off first, then 'g', then on */
    uint32_t first = 0, trig = 0; t0 = ccount();
    rc = rf_rx_capture_trigger(RF_RATE_16M, RF_FMT_IQ8, 90, 100000, 400000, p, 10000, &first, &trig);
    psram_invalidate(p, 800000);
    uint32_t pre_i = (trig + 400000 - 50000) % 400000, post_i = (trig + 50000) % 400000;
    int pk_before = 0, pk_after = 0;
    for (uint32_t k = 1; k <= 1000; k++) { uint16_t v = p[(trig + 400000 - k) % 400000]; int a = abs((int8_t)(v & 0xFF)) + abs((int8_t)(v >> 8)); if (a > pk_before) pk_before = a; }
    for (uint32_t k = 0; k < 64; k++) { uint16_t v = p[(trig + k) % 400000]; int a = abs((int8_t)(v & 0xFF)) + abs((int8_t)(v >> 8)); if (a > pk_after) pk_after = a; }
    printf("trigger check (8-bit scale, level 90 = %d): peak |I|+|Q| in 1000 samples before %d, in 64 from the trigger %d\n", 90 / 4, pk_before, pk_after);
    printf("trigger rc %d after %.0f ms: first %lu trig %lu | 3 ms before trigger %.1f dB, 3 ms after %.1f dB\n", rc, (ccount() - t0) / 240e3, (unsigned long)first, (unsigned long)trig,
           tone_db8(p + (pre_i < 400000 - 4096 ? pre_i : 0), 4096, 1e6f, 16e6f), tone_db8(p + (post_i < 400000 - 4096 ? post_i : 0), 4096, 1e6f, 16e6f));
    printf("=== done\n");
    for (;;) {}
}
