/* IDF sibling of components/lib/test_apps/dsp_lib: ESP-DSP 1.8.2 (S3 "aes3" optimised where it has them), same inputs (LCG), cycles
 * per call (best of 3), plus probe values of each output for the cross-check with our results (tools: compare). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_cpu.h"
#include "esp_dsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define A16 __attribute__((aligned(16)))
static float x[2 * 4096] A16, y[2 * 4096] A16, z[4096 + 128] A16, h[64] A16, dl[128] A16, w5[5], st[2];
static int16_t s[2 * 1024] A16, s2[1024] A16, so[1024] A16, hs[32] A16, dls[64] A16;
static float ma[64 * 64] A16, mb[64 * 64] A16, mc[64 * 64] A16;
static int16_t mas[64 * 64] A16, mbs[64 * 64] A16, mcs[64 * 64] A16;
static uint32_t seed;
static float rnd(void) { seed = seed * 1103515245u + 12345u; return (int32_t)(seed >> 8 & 0xFFFF) / 32768.0f - 1.0f; }
static void fill(float *p, int n, uint32_t sd) { seed = sd; for (int i = 0; i < n; i++) p[i] = rnd(); }
static void fills(int16_t *p, int n, uint32_t sd) { seed = sd; for (int i = 0; i < n; i++) p[i] = (int16_t)(rnd() * 8000); }
#define BEST(label, prep, call, probe) do { uint32_t b = ~0u; for (int r = 0; r < 3; r++) { prep; uint32_t t = esp_cpu_get_cycle_count(); call; t = esp_cpu_get_cycle_count() - t; if (t < b) b = t; } \
    printf("idf %-28s %8lu cycles  probe %s\n", label, (unsigned long)b, probe); } while (0)
static char pb[96];
static const char *pf(const float *p, int i0, int i1, int i2) { snprintf(pb, sizeof pb, "%.5g %.5g %.5g", p[i0], p[i1], p[i2]); return pb; }
static const char *ps(const int16_t *p, int i0, int i1, int i2) { snprintf(pb, sizeof pb, "%d %d %d", p[i0], p[i1], p[i2]); return pb; }

void app_main(void)
{
    vTaskDelay(100);
    dsps_fft2r_init_fc32(NULL, 4096); dsps_fft4r_init_fc32(NULL, 4096); dsps_fft2r_init_sc16(NULL, 1024);
    static const int fn[3] = { 256, 1024, 4096 };
    for (int k = 0; k < 3; k++) {
        int n = fn[k]; char l[40];
        snprintf(l, sizeof l, "fft2r_fc32+bitrev %d", n);
        BEST(l, fill(x, 2 * n, 7), (dsps_fft2r_fc32(x, n), dsps_bit_rev_fc32(x, n)), pf(x, 2, 3, 2 * n - 1));
        snprintf(l, sizeof l, "fft4r_fc32+bitrev %d", n);
        BEST(l, fill(x, 2 * n, 7), (dsps_fft4r_fc32(x, n), dsps_bit_rev4r_fc32(x, n)), pf(x, 2, 3, 2 * n - 1));
    }
    BEST("real fft 1024 (fft2r 512+c2r)", fill(x, 1024, 9), (dsps_fft2r_fc32(x, 512), dsps_bit_rev_fc32(x, 512), dsps_cplx2reC_fc32(x, 512)), pf(x, 2, 3, 1023));
    BEST("fft2r_sc16+bitrev 1024", fills(s, 2048, 11), (dsps_fft2r_sc16(s, 1024), dsps_bit_rev_sc16_ansi(s, 1024)), ps(s, 2, 3, 2047));
    BEST("wind_hann 1024", , dsps_wind_hann_f32(z, 1024), pf(z, 1, 300, 512));
    BEST("wind_blackman 1024", , dsps_wind_blackman_f32(z, 1024), pf(z, 1, 300, 512));
    BEST("wind_flat_top 1024", , dsps_wind_flat_top_f32(z, 1024), pf(z, 1, 300, 512));
    fir_f32_t fir; fill(h, 64, 13);
    BEST("fir_f32 64 taps x 1024", (fill(x, 1024, 15), memset(dl, 0, sizeof dl), dsps_fir_init_f32(&fir, h, dl, 64)), dsps_fir_f32(&fir, x, y, 1024), pf(y, 5, 500, 1023));
    fir_s16_t firs; fills(hs, 32, 17);
    BEST("fird_s16 32 taps /4 x 1024", (fills(s, 1024, 19), memset(dls, 0, sizeof dls), dsps_fird_init_s16(&firs, hs, dls, 32, 4, 0, 0)), dsps_fird_s16(&firs, s, so, 256), ps(so, 5, 100, 255));
    float co[5] = { 0.2066f, 0.4131f, 0.2066f, -0.3695f, 0.1958f };
    BEST("biquad_f32 x 1024", (fill(x, 1024, 21), st[0] = st[1] = 0), dsps_biquad_f32(x, y, 1024, co, st), pf(y, 5, 500, 1023));
    fill(x, 1024, 23); fill(y, 1024, 25);
    BEST("add_f32 1024", , dsps_add_f32(x, y, z, 1024, 1, 1, 1), pf(z, 1, 500, 1023));
    BEST("mul_f32 1024", , dsps_mul_f32(x, y, z, 1024, 1, 1, 1), pf(z, 1, 500, 1023));
    BEST("mulc_f32 1024", , dsps_mulc_f32(x, z, 1024, 0.75f, 1, 1), pf(z, 1, 500, 1023));
    fills(s, 1024, 27); fills(s2, 1024, 29);
    BEST("add_s16 1024", , dsps_add_s16(s, s2, so, 1024, 1, 1, 1, 0), ps(so, 1, 500, 1023));
    BEST("mul_s16 1024", , dsps_mul_s16(s, s2, so, 1024, 1, 1, 1, 15), ps(so, 1, 500, 1023));
    float df; BEST("dotprod_f32 1024", , dsps_dotprod_f32(x, y, &df, 1024), pf(&df, 0, 0, 0));
    fill(ma, 4096, 31); fill(mb, 4096, 33);
    BEST("mult_f32 16x16x16", , dspm_mult_f32(ma, mb, mc, 16, 16, 16), pf(mc, 0, 100, 255));
    BEST("mult_f32 64x64x64", , dspm_mult_f32(ma, mb, mc, 64, 64, 64), pf(mc, 0, 1000, 4095));
    fills(mas, 4096, 35); fills(mbs, 4096, 37);
    BEST("mult_s16 64x64x64 >>15", , dspm_mult_s16(mas, mbs, mcs, 64, 64, 64, 0), ps(mcs, 0, 1000, 4095));
    fill(x, 1024, 39); fill(h, 64, 41);
    BEST("conv_f32 1024 * 64", , dsps_conv_f32(x, 1024, h, 64, z), pf(z, 5, 500, 1086));
    BEST("corr_f32 1024 . 64", , dsps_corr_f32(x, 1024, h, 64, z), pf(z, 5, 500, 960));
    BEST("dct_f32 256", fill(x, 256, 43), dsps_dct_f32(x, 256), pf(x, 1, 100, 255));
    printf("idf_dsplib done\n");
    for (;;) vTaskDelay(1000);
}
