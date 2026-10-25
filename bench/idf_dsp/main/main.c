/* IDF sibling of components/lib/test_apps/dsp_bench: Espressif's ESP-DSP (S3 vector
 * versions) on the same data sizes. Cycles per call. */
#include <stdio.h>
#include <string.h>
#include "esp_cpu.h"
#include "esp_dsp.h"
#include "dsps_mem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define N 1024
static int16_t a16[N] __attribute__((aligned(16))), b16[N] __attribute__((aligned(16))), o16[N] __attribute__((aligned(16)));
static float af[N] __attribute__((aligned(16))), bf[N] __attribute__((aligned(16)));
static uint8_t src[16384] __attribute__((aligned(16))), dst[16384] __attribute__((aligned(16)));
#define TIME(e) ({ uint32_t _t = esp_cpu_get_cycle_count(); e; esp_cpu_get_cycle_count() - _t; })

void app_main(void)
{
    vTaskDelay(10);
    uint32_t x = 12345;
    for (int i = 0; i < N; i++) {
        x = x * 1103515245u + 12345u; a16[i] = (int16_t)(x >> 16) / 8;
        x = x * 1103515245u + 12345u; b16[i] = (int16_t)(x >> 16) / 8;
        af[i] = a16[i] * 0.001f; bf[i] = b16[i] * 0.001f;
    }
    for (int i = 0; i < 16384; i++) src[i] = i * 7;
    printf("\n=== ESP-DSP on the same sizes (cycles per call) ===\n");
    int16_t d16; float df;
    uint32_t t;
    for (int k = 0; k < 2; k++) {          /* second pass = warm (flash cache) */
        t = TIME(dsps_dotprod_s16(a16, b16, &d16, N, 15));
        if (k) printf("  dsps_dotprod_s16 x%d (int16 out, shift 15): %lu\n", N, t);
        t = TIME(dsps_add_s16(a16, b16, o16, N, 1, 1, 1, 0));
        if (k) printf("  dsps_add_s16 x%d:                          %lu\n", N, t);
        t = TIME(dsps_dotprod_f32(af, bf, &df, N));
        if (k) printf("  dsps_dotprod_f32 x%d:                      %lu\n", N, t);
        t = TIME(dsps_memcpy(dst, src, sizeof src));
        if (k) printf("  dsps_memcpy 16 KB:                          %lu (%lu MB/s)\n", t, 16384u * 240 / t);
        t = TIME(memcpy(dst, src, sizeof src));
        if (k) printf("  memcpy (newlib/ROM) 16 KB:                  %lu (%lu MB/s)\n", t, 16384u * 240 / t);
    }
    /* decimating FIR (sibling of rf_rx_capture_dec): I and Q as two dsps_fird_s16 instances, 4096 inputs */
    static int16_t ii[4096] __attribute__((aligned(16))), qq[4096] __attribute__((aligned(16))), oi[512], oq[512];
    static int16_t h[128] __attribute__((aligned(16))), dli[128 + 16] __attribute__((aligned(16))), dlq[128 + 16] __attribute__((aligned(16)));
    for (int i = 0; i < 4096; i++) { x = x * 1103515245u + 12345u; ii[i] = (int16_t)(x >> 22); x = x * 1103515245u + 12345u; qq[i] = (int16_t)(x >> 22); }
    for (int i = 0; i < 128; i++) h[i] = 256;
    static const int cfg[2][2] = { { 8, 48 }, { 16, 128 } };
    for (int c = 0; c < 2; c++) {
        fir_s16_t fi, fq; int D = cfg[c][0], T = cfg[c][1];
        dsps_fird_init_s16(&fi, h, dli, T, D, 0, 15); dsps_fird_init_s16(&fq, h, dlq, T, D, 0, 15);
        for (int k = 0; k < 2; k++) {
            t = TIME({ dsps_fird_s16(&fi, ii, oi, 4096); dsps_fird_s16(&fq, qq, oq, 4096);   /* len = input samples */ });
            if (k) printf("  dsps_fird_s16 I+Q decim %d, %d taps: %.2f cycles/input sample (filter only, int16 input)\n", D, T, t / 4096.0);
        }
        dsps_fird_s16_aexx_free(&fi); dsps_fird_s16_aexx_free(&fq);
    }
    printf("=== done ===\n");
}
