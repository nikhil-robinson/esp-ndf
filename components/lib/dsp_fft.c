/* FFT (complex float, real float, complex int16), windows and DCT-II. See dsp.h.
 * Float FFT: bit reversal, then radix-4 passes (3 complex multiplies per 4 points), one
 * radix-2 pass first when log2(n) is odd. One shared twiddle table W_4096^i, i < 3072 (24 KB), built once. */
#include "dsp.h"
#include <math.h>

#ifndef SOC_DSP_F32_ASM
#define SOC_DSP_F32_ASM SOC_CPU_HAS_PIE                          /* float kernels in assembly (S3: FPU fed by PIE loads); the P4 compiler does as well */
#endif
#if SOC_DSP_F32_ASM
void dsp_r4pass_f32(float *p, int h, const float *tw, int st_bytes);   /* components/lib/dsp_f32.S */
void dsp_r4first_f32(float *x, int groups);
#endif
#if SOC_CPU_HAS_PIE
void dsp_fft_s16_stage(int16_t *x, int n2, int blocks, const int16_t *wbr);
void dsp_fft_s16_last2(int16_t *x, int n, const int16_t *wbr);
#endif

#define FFT_MAX 4096
static float tw[2 * (3 * FFT_MAX / 4)] __attribute__((aligned(16)));       /* cos, -sin interleaved: W_4096^i = tw[2i] + j tw[2i+1], i < 3072 */
static int16_t tws[FFT_MAX] __attribute__((aligned(16)));   /* (cos, sin) Q15 of W^-i, bit-reversed index (11 bits) */
static int tw_ready;

static void tw_init(void)
{
    if (tw_ready) return;
    for (int i = 0; i < 3 * FFT_MAX / 4; i++) {
        double a = 2.0 * M_PI * i / FFT_MAX;
        tw[2 * i] = (float)cos(a); tw[2 * i + 1] = (float)-sin(a);
        if (i < FFT_MAX / 2) {
            int r = 0; for (int b = 0; b < 11; b++) r |= ((i >> b) & 1) << (10 - b);
            tws[2 * r] = (int16_t)lrint(cos(a) * 32767.0); tws[2 * r + 1] = (int16_t)lrint(sin(a) * 32767.0);
        }
    }
    tw_ready = 1;
}

static int log2i(int n) { int l = 0; while ((1 << l) < n) l++; return (1 << l) == n ? l : -1; }

/* bit reversal by a cached list of swap pairs for the last size used (2016 pairs at 4096: 8 KB) */
static uint16_t swp[2 * 2016];
static int swp_n, swp_cnt;
static void swaps(int n, int lg)
{
    if (swp_n != n) {
        swp_cnt = 0;
        for (int i = 1; i < n; i++) {
            int j = 0;
            for (int b = 0; b < lg; b++) j |= ((i >> b) & 1) << (lg - 1 - b);
            if (i < j) { swp[2 * swp_cnt] = i; swp[2 * swp_cnt + 1] = j; swp_cnt++; }
        }
        swp_n = n;
    }
}
static void bitrev_f32(float *x, int n, int lg)
{
    swaps(n, lg);
    uint64_t *c = (uint64_t *)x;                  /* one complex = 8 bytes */
    for (int k = 0; k < swp_cnt; k++) { int i = swp[2 * k], j = swp[2 * k + 1]; uint64_t t = c[i]; c[i] = c[j]; c[j] = t; }
}

/* in-place forward FFT on bit-reversed input. Radix-4 DIT passes: with radix-2 bit reversal the sub-DFTs of a
 * group sit at offsets 0, h, 2h, 3h for input residues 0, 2, 1, 3 (mod 4), so the twiddles are 1, W^2k, W^k, W^3k. */
static void fft_core(float *x, int n, int lg)
{
    int h = 1;
    if (lg & 1) {                                 /* one radix-2 pass */
        for (int i = 0; i < 2 * n; i += 4) {
            float ar = x[i], ai = x[i + 1], br = x[i + 2], bi = x[i + 3];
            x[i] = ar + br; x[i + 1] = ai + bi; x[i + 2] = ar - br; x[i + 3] = ai - bi;
        }
        h = 2;
    }
#if SOC_DSP_F32_ASM
    if (h == 1 && n >= 4 && !((uintptr_t)x & 15)) { dsp_r4first_f32(x, n / 4); h = 4; }
#endif
    for (; h < n; h *= 4) {
        int st = FFT_MAX / (4 * h);               /* W_4h^k = W_4096^(k st) */
#if SOC_DSP_F32_ASM
        if (!((uintptr_t)x & 7)) { for (int b = 0; b < n; b += 4 * h) dsp_r4pass_f32(x + 2 * b, h, tw, 8 * st); continue; }
#endif
        for (int k = 0; k < h; k++) {
            const float *w1 = tw + 2 * k * st, *w2 = tw + 4 * k * st, *w3 = tw + 6 * k * st;
            float w1r = w1[0], w1i = w1[1], w2r = w2[0], w2i = w2[1], w3r = w3[0], w3i = w3[1];
            for (int b = 0; b < n; b += 4 * h) {
                float *p = x + 2 * (b + k);
                float ar = p[0], ai = p[1];
                float br = p[2 * h], bi = p[2 * h + 1], cr = p[4 * h], ci = p[4 * h + 1], dr = p[6 * h], di = p[6 * h + 1];
                float b2r = br * w2r - bi * w2i, b2i = br * w2i + bi * w2r;      /* residue 2: W^2k */
                float c1r = cr * w1r - ci * w1i, c1i = cr * w1i + ci * w1r;      /* residue 1: W^k  */
                float d3r = dr * w3r - di * w3i, d3i = dr * w3i + di * w3r;      /* residue 3: W^3k */
                float t0r = ar + b2r, t0i = ai + b2i, t1r = ar - b2r, t1i = ai - b2i;
                float t2r = c1r + d3r, t2i = c1i + d3i, t3r = c1r - d3r, t3i = c1i - d3i;
                p[0] = t0r + t2r; p[1] = t0i + t2i; p[4 * h] = t0r - t2r; p[4 * h + 1] = t0i - t2i;
                p[2 * h] = t1r + t3i; p[2 * h + 1] = t1i - t3r;                 /* t1 - j t3 */
                p[6 * h] = t1r - t3i; p[6 * h + 1] = t1i + t3r;
            }
        }
    }
}

int dsp_fft_f32(float *x, int n)
{
    int lg = log2i(n);
    if (lg < 1 || n > FFT_MAX) return -1;
    tw_init(); bitrev_f32(x, n, lg); fft_core(x, n, lg);
    return 0;
}

int dsp_ifft_f32(float *x, int n)
{
    int lg = log2i(n);
    if (lg < 1 || n > FFT_MAX) return -1;
    for (int i = 1; i < 2 * n; i += 2) x[i] = -x[i];                 /* ifft(x) = conj(fft(conj x)) / n */
    tw_init(); bitrev_f32(x, n, lg); fft_core(x, n, lg);
    float s = 1.0f / n;
    for (int i = 0; i < 2 * n; i += 2) { x[i] *= s; x[i + 1] *= -s; }
    return 0;
}

/* real FFT of n samples through an n/2 complex FFT + split: out x[0] = X0, x[1] = X(n/2), then X1.. X(n/2-1) */
int dsp_rfft_f32(float *x, int n)
{
    int m = n / 2;
    if (dsp_fft_f32(x, m)) return -1;
    float z0r = x[0], z0i = x[1];
    x[0] = z0r + z0i; x[1] = z0r - z0i;
    int st = FFT_MAX / n;
    for (int k = 1; k <= m / 2; k++) {
        int j = m - k;
        float ar = x[2 * k], ai = x[2 * k + 1], br = x[2 * j], bi = -x[2 * j + 1];     /* Z[k], conj Z[m-k] */
        float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi), or_ = 0.5f * (ai - bi), oi = -0.5f * (ar - br);   /* even, odd */
        float wr = tw[2 * k * st], wi = tw[2 * k * st + 1];
        float tr = or_ * wr - oi * wi, ti = or_ * wi + oi * wr;
        x[2 * k] = er + tr; x[2 * k + 1] = ei + ti;
        if (j != k) { x[2 * j] = er - tr; x[2 * j + 1] = -(ei - ti); }
    }
    return 0;
}

/* complex int16 FFT, output = DFT / n, as ESP-DSP dsps_fft2r_sc16 (+ bit reversal): natural-order radix-2 stages
 * with bit-reversed twiddles (one table serves every size), all stages on PIE (scalar C when unaligned), then the
 * swap-list bit reversal (32-bit moves). PIE halves by truncation (as ESP-DSP's aes3 code): ~0.5 LSB bias per
 * stage at DC. */
static inline int16_t bf(int32_t a, int32_t t) { return (int16_t)((a * 32767 + t + 0x7FFF) >> 16); }
int dsp_fft_s16(int16_t *x, int n)
{
    int lg = log2i(n);
    if (lg < 1 || n > FFT_MAX) return -1;
    tw_init();
    int n2 = n / 2, blocks = 1;
#if SOC_CPU_HAS_PIE
    if (!((uintptr_t)x & 15) && n >= 8) {
        for (; n2 >= 4; n2 /= 2, blocks *= 2) dsp_fft_s16_stage(x, n2, blocks, tws);
        dsp_fft_s16_last2(x, n, tws); n2 = 0;
    }
#endif
    for (; n2 >= 1; n2 /= 2, blocks *= 2)
        for (int j = 0; j < blocks; j++) {
            int32_t c = tws[2 * j], s = tws[2 * j + 1];
            int16_t *p = x + 4 * n2 * j;
            for (int i = 0; i < n2; i++) {
                int16_t *a = p + 2 * i, *m = a + 2 * n2;
                int32_t tr = c * m[0] + s * m[1], ti = c * m[1] - s * m[0];
                int32_t ar = a[0], ai = a[1];
                int32_t hr = ar * 32767 + 0x7FFF, hi = ai * 32767 + 0x7FFF;      /* the same sums as bf(): the a * 32767 product once for both outputs */
                m[0] = (int16_t)((hr - tr) >> 16); m[1] = (int16_t)((hi - ti) >> 16); a[0] = (int16_t)((hr + tr) >> 16); a[1] = (int16_t)((hi + ti) >> 16);
            }
        }
    swaps(n, lg);
    uint32_t *c32 = (uint32_t *)x;                /* one int16 complex = 4 bytes */
    for (int k = 0; k < swp_cnt; k++) { int i = swp[2 * k], j = swp[2 * k + 1]; uint32_t t = c32[i]; c32[i] = c32[j]; c32[j] = t; }
    return 0;
}

/* windows: cos(i t) by float rotation, re-seeded from cosf/sinf every 64 samples (error < 1e-6); numpy/ESP-DSP
 * definitions (len - 1 denominator); cos(2x), cos(3x), cos(4x) from cos(x) by Chebyshev recurrence */
static void win(float *w, int n, const float *a, int terms)
{
    float t = 2.0f * (float)M_PI / (n - 1), c1 = cosf(t), s1 = sinf(t), cr = 1, ci = 0;
    for (int i = 0; i < n; i++) {
        if (!(i & 63)) { cr = cosf(t * i); ci = sinf(t * i); }
        float c2 = 2 * cr * cr - 1, v = a[0] - a[1] * cr;
        if (terms > 2) v += a[2] * c2;
        if (terms > 3) { float c3 = 2 * cr * c2 - cr, c4 = 2 * c2 * c2 - 1; v += -a[3] * c3 + a[4] * c4; }
        w[i] = v;
        float r = cr * c1 - ci * s1; ci = cr * s1 + ci * c1; cr = r;
    }
}
void dsp_win_hann_f32(float *w, int n)     { static const float a[] = { 0.5f, 0.5f }; win(w, n, a, 2); }
void dsp_win_blackman_f32(float *w, int n) { static const float a[] = { 0.42f, 0.5f, 0.08f }; win(w, n, a, 3); }
void dsp_win_flattop_f32(float *w, int n)  { static const float a[] = { 0.21557895f, 0.41663158f, 0.277263158f, 0.083578947f, 0.006947368f }; win(w, n, a, 5); }

/* DCT-II (unnormalised, X[k] = sum x[i] cos(pi k (2i+1) / 2n)), Makhoul: reorder, one n-point complex FFT, rotate */
int dsp_dct_f32(float *x, float *work, int n)
{
    if (log2i(n) < 1 || n > FFT_MAX) return -1;
    for (int i = 0; i < n / 2; i++) { work[2 * i] = x[2 * i]; work[2 * (n - 1 - i)] = x[2 * i + 1]; }
    for (int i = 0; i < n; i++) work[2 * i + 1] = 0;
    dsp_fft_f32(work, n);
    int st = FFT_MAX / (4 * n);
    for (int k = 0; k < n; k++) {                 /* X[k] = Re(V[k] * exp(-j pi k / 2n)) */
        float c, s;
        if (st) { c = tw[2 * k * st]; s = tw[2 * k * st + 1]; } else { c = cosf((float)M_PI * k / (2 * n)); s = -sinf((float)M_PI * k / (2 * n)); }
        x[k] = work[2 * k] * c - work[2 * k + 1] * s;
    }
    return 0;
}
