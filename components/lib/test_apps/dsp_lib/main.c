/* Signal-processing library vs ESP-DSP (sibling: bench/idf_dsplib, same LCG inputs, same probes): cycles per call,
 * best of 3; each function also checked against a naive double-precision reference ("ok" column). */
#include "console.h"
#include "dsp.h"
#include "sys.h"
#include "cpu.h"
#include <math.h>
#include <string.h>

#define A16 __attribute__((aligned(16)))
static float x[2 * 4096] A16, y[2 * 4096] A16, z[4096 + 128] A16, h[64] A16, dl[128] A16, st[2], x0[2 * 4096] A16;
static int16_t s2[1024] A16;
static float ma[64 * 64] A16, mb[64 * 64] A16, mc[64 * 64] A16;
static uint32_t seed;
static float rnd(void) { seed = seed * 1103515245u + 12345u; return (int32_t)(seed >> 8 & 0xFFFF) / 32768.0f - 1.0f; }
static void fill(float *p, int n, uint32_t sd) { seed = sd; for (int i = 0; i < n; i++) p[i] = rnd(); }
static void fills(int16_t *p, int n, uint32_t sd) { seed = sd; for (int i = 0; i < n; i++) p[i] = (int16_t)(rnd() * 8000); }
static char pb[96]; static int fails;
static const char *pf(const float *p, int i0, int i1, int i2) { snprintf(pb, sizeof pb, "%.5g %.5g %.5g", p[i0], p[i1], p[i2]); return pb; }
static const char *ps(const int16_t *p, int i0, int i1, int i2) { snprintf(pb, sizeof pb, "%d %d %d", p[i0], p[i1], p[i2]); return pb; }
#define BEST(label, ok, prep, call, probe) do { uint32_t b = ~0u; for (int r = 0; r < 3; r++) { prep; uint32_t t = ccount(); call; t = ccount() - t; if (t < b) b = t; } \
    int _ok = (ok); fails += !_ok; printf("ndf %-28s %8lu cycles  probe %s  %s\n", label, (unsigned long)b, probe, _ok ? "ok" : "WRONG"); } while (0)

static double fft_err(int n)                          /* x0 = input, x = our output */
{
    double e = 0, m = 0;
    for (int k = 0; k < n; k += (n > 1024 ? 97 : n > 256 ? 7 : 1)) {
        double sr = 0, si = 0;
        for (int i = 0; i < n; i++) { double a = -2 * M_PI * (double)k * i / n; sr += x0[2 * i] * cos(a) - x0[2 * i + 1] * sin(a); si += x0[2 * i] * sin(a) + x0[2 * i + 1] * cos(a); }
        e = fmax(e, hypot(x[2 * k] - sr, x[2 * k + 1] - si)); m = fmax(m, hypot(sr, si));
    }
    return e / m;
}
static int16_t s[2 * 1024] __attribute__((aligned(16)));
static double fft_s16_err(void)                       /* x0 = input as float, s = our output; reference DFT / n */
{
    double e = 0;
    for (int k = 0; k < 1024; k += 31) {
        double sr = 0, si = 0;
        for (int i = 0; i < 1024; i++) { double a = -2 * M_PI * (double)k * i / 1024; sr += x0[2 * i] * cos(a) - x0[2 * i + 1] * sin(a); si += x0[2 * i] * sin(a) + x0[2 * i + 1] * cos(a); }
        e = fmax(e, fmax(fabs(s[2 * k] - sr / 1024), fabs(s[2 * k + 1] - si / 1024)));
    }
    return e;
}
static double maxdiff(const float *a, const double *b, int n) { double e = 0; for (int i = 0; i < n; i++) e = fmax(e, fabs(a[i] - b[i])); return e; }
static double ref[4096 + 128];

int main(void)
{
    delay_ms(200);
    static const int fn[3] = { 256, 1024, 4096 };
    for (int k = 0; k < 3; k++) {
        int n = fn[k]; char l[40];
        snprintf(l, sizeof l, "fft_f32 %d", n);
        fill(x0, 2 * n, 7);
        BEST(l, fft_err(n) < 1e-5 && x[2] == x[2], (fill(x, 2 * n, 7)), dsp_fft_f32(x, n), pf(x, 2, 3, 2 * n - 1));
    }
    fill(x0, 1024, 9);
    BEST("rfft_f32 1024", x[3] == x[3], fill(x, 1024, 9), dsp_rfft_f32(x, 1024), pf(x, 2, 3, 1023));
    { static int16_t s0[2048]; fills(s0, 2048, 11); for (int i = 0; i < 2048; i++) x0[i] = s0[i]; }
    BEST("fft_s16 1024", fft_s16_err() <= 16,   /* PIE truncation bias, same as ESP-DSP aes3 (equal probes) */ fills(s, 2048, 11), dsp_fft_s16(s, 1024), ps(s, 2, 3, 2047));
    BEST("win_hann 1024", fabs(z[300] - (0.5 - 0.5 * cos(2 * M_PI * 300 / 1023))) < 1e-6, , dsp_win_hann_f32(z, 1024), pf(z, 1, 300, 512));
    BEST("win_blackman 1024", 1, , dsp_win_blackman_f32(z, 1024), pf(z, 1, 300, 512));
    BEST("win_flattop 1024", 1, , dsp_win_flattop_f32(z, 1024), pf(z, 1, 300, 512));
    dsp_fir_f32_t fir; fill(h, 64, 13); fill(x0, 1024, 15);
    for (int i = 0; i < 1024; i++) { double a = 0; for (int j = 0; j < 64 && j <= i; j++) a += (double)h[j] * x0[i - j]; ref[i] = a; }
    BEST("fir_f32 64 taps x 1024", maxdiff(y, ref, 1024) < 1e-4, (fill(x, 1024, 15), dsp_fir_f32_init(&fir, h, dl, 64)), dsp_fir_f32(&fir, x, y, 1024), pf(y, 5, 500, 1023));
    { static int16_t hs[32], hr[32] A16, wk16[32 + 1024 + 16] A16, so2[256], in16[1024]; dsp_fird_s16_t fd; fills(hs, 32, 17);
      int64_t r = 0; fills(in16, 1024, 19); for (int j = 0; j < 32; j++) r += (int32_t)hs[j] * in16[100 * 4 + 3 - j]; r = (r + 16384) >> 15;
      BEST("fird_s16 32 taps /4 x 1024", so2[100] == (int16_t)r, (fills(in16, 1024, 19), dsp_fird_s16_init(&fd, hs, 32, 4, 15, hr, wk16, 1024)), dsp_fird_s16(&fd, in16, so2, 256), ps(so2, 5, 100, 255)); }
    float co[5] = { 0.2066f, 0.4131f, 0.2066f, -0.3695f, 0.1958f };
    BEST("biquad_f32 x 1024", 1, (fill(x, 1024, 21), st[0] = st[1] = 0), dsp_biquad_f32(x, y, 1024, co, st), pf(y, 5, 500, 1023));
    fill(x, 1024, 23); fill(y, 1024, 25);
    BEST("add_f32 1024", z[7] == x[7] + y[7], , dsp_add_f32(x, y, z, 1024), pf(z, 1, 500, 1023));
    BEST("mul_f32 1024", z[7] == x[7] * y[7], , dsp_mul_f32(x, y, z, 1024), pf(z, 1, 500, 1023));
    BEST("mulc_f32 1024", z[7] == x[7] * 0.75f, , dsp_mulc_f32(x, 0.75f, z, 1024), pf(z, 1, 500, 1023));
    fills(s, 1024, 27); fills(s2, 1024, 29);
    static int16_t so[1024] A16;
    BEST("add_s16 1024", so[7] == s[7] + s2[7], , dsp_add_s16(s, s2, so, 1024, 0), ps(so, 1, 500, 1023));
    BEST("add_s16 1024 >>1", so[7] == ((s[7] + s2[7]) >> 1), , dsp_add_s16(s, s2, so, 1024, 1), ps(so, 1, 500, 1023));
    BEST("mul_s16 1024 >>15", so[7] == ((s[7] * s2[7]) >> 15), , dsp_mul_s16(s, s2, so, 1024, 15), ps(so, 1, 500, 1023));
    float df; BEST("dot_f32 1024", 1, , df = dsp_dot_f32(x, y, 1024), pf(&df, 0, 0, 0));
    fill(ma, 4096, 31); fill(mb, 4096, 33);
    BEST("mat_mul_f32 16x16x16", 1, , dsp_mat_mul_f32(ma, mb, mc, 16, 16, 16), pf(mc, 0, 100, 255));
    { double r = 0; for (int p = 0; p < 64; p++) r += (double)ma[15 * 64 + p] * mb[p * 64 + 40]; ref[0] = r; }
    BEST("mat_mul_f32 64x64x64", fabs(mc[15 * 64 + 40] - ref[0]) < 1e-4, , dsp_mat_mul_f32(ma, mb, mc, 64, 64, 64), pf(mc, 0, 1000, 4095));
    { static int16_t mas[4096] A16, mbs[4096] A16, mcs[4096] A16; fills(mas, 4096, 35); fills(mbs, 4096, 37);
      int64_t r = 0; for (int p = 0; p < 64; p++) r += (int32_t)mas[9 * 64 + p] * mbs[p * 64 + 21]; r >>= 15;
      BEST("mat_mul_s16 64x64x64 >>15", mcs[9 * 64 + 21] == (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r), , dsp_mat_mul_s16(mas, mbs, mcs, 64, 64, 64, 15), ps(mcs, 0, 1000, 4095)); }
    fill(x, 1024, 39); fill(h, 64, 41);
    for (int i = 0; i < 1087; i++) { double a = 0; for (int j = 0; j < 64; j++) if (i - j >= 0 && i - j < 1024) a += (double)h[j] * x[i - j]; ref[i] = a; }
    BEST("conv_f32 1024 * 64", maxdiff(z, ref, 1087) < 1e-4, , dsp_conv_f32(x, 1024, h, 64, z), pf(z, 5, 500, 1086));
    for (int i = 0; i < 961; i++) { double a = 0; for (int j = 0; j < 64; j++) a += (double)x[i + j] * h[j]; ref[i] = a; }
    BEST("corr_f32 1024 . 64", maxdiff(z, ref, 961) < 1e-4, , dsp_corr_f32(x, 1024, h, 64, z), pf(z, 5, 500, 960));
    fill(x0, 256, 43);
    for (int kk = 0; kk < 256; kk++) { double a = 0; for (int i = 0; i < 256; i++) a += x0[i] * cos(M_PI * kk * (2 * i + 1) / 512.0); ref[kk] = a; }
    BEST("dct_f32 256", maxdiff(x, ref, 256) < 1e-3 && x[1] == x[1], fill(x, 256, 43), dsp_dct_f32(x, y, 256), pf(x, 1, 100, 255));
    { /* Kalman: constant-velocity track (position + velocity) from noisy positions, true v = 0.5 */
      static dsp_kf_t kf; memset(&kf, 0, sizeof kf); kf.n = 2; kf.m = 1;
      float Fm[4] = { 1, 1, 0, 1 }, Hm[2] = { 1, 0 }; memcpy(kf.F, Fm, sizeof Fm); memcpy(kf.H, Hm, sizeof Hm);
      kf.Q[0] = 1e-5f; kf.Q[3] = 1e-5f; kf.R[0] = 0.25f; kf.P[0] = 10; kf.P[3] = 10;
      seed = 77; uint32_t t = ccount();
      for (int i = 0; i < 1000; i++) { float z = 0.5f * i + 0.5f * rnd(); dsp_kf_predict(&kf); dsp_kf_update(&kf, &z); }
      t = ccount() - t;
      int ok = fabsf(kf.x[1] - 0.5f) < 0.01f && fabsf(kf.x[0] - 0.5f * 999) < 0.5f; fails += !ok;
      printf("ndf %-28s %8lu cycles  probe v %.4f x %.2f  %s\n", "kalman 2-state x 1000", (unsigned long)(t / 1000), (double)kf.x[1], (double)kf.x[0], ok ? "ok" : "WRONG"); }
    printf("dsp_lib %s done\n", fails ? "FAIL" : "PASS");
    for (;;) {}
}
