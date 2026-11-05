/* Filters, vector and matrix math, convolution (float FPU code; int16 via the PIE kernels in dsp.S). See dsp.h. */
#include "dsp.h"
#include <string.h>

#ifndef SOC_DSP_F32_ASM
#define SOC_DSP_F32_ASM SOC_CPU_HAS_PIE                          /* float kernels in assembly (S3: FPU fed by PIE loads); the P4 compiler does as well */
#endif
#if SOC_DSP_F32_ASM                                               /* components/lib/dsp_f32.S */
void dsp_vadd8_f32(const float *a, const float *b, float *y, int blocks8);
void dsp_vsub8_f32(const float *a, const float *b, float *y, int blocks8);
void dsp_vmul8_f32(const float *a, const float *b, float *y, int blocks8);
void dsp_vmulc8_f32(const float *a, float *y, int blocks8, uint32_t c_bits);
void dsp_mm4_f32(const float *A, const float *B, float *C, int m, int n, int k);
void dsp_biquad_f32_asm(const float *x, float *y, int n, const float *c, float *w);
#endif
#if SOC_CPU_HAS_PIE
void dsp_vadds16_sh(const int16_t *a, const int16_t *b, int16_t *y, int blocks16, int shift);
void dsp_vmuls16_sh(const int16_t *a, const int16_t *b, int16_t *y, int blocks16, int shift);
void dsp_mm8_s16(const int16_t *A, const int16_t *B, int16_t *C, int m, int n, int k, int shift);
int32_t dsp_dotu_s16(const int16_t *x, const int16_t *h, int groups8);
void dsp_fird_core_s16(const int16_t *x0, const int16_t *hr, int16_t *out, int n_out, int step_bytes, int groups8, int shift, int rnd);
static inline bool al3(const void *a, const void *b, const void *c) { return !((((uintptr_t)a) | (uintptr_t)b | (uintptr_t)c) & 15); }
#endif

/* y[i] = sum_j h[j] x[i-j] for i in [0, n): four outputs per pass; the input window lives in four registers whose
 * roles rotate over an unroll of 4 taps (no moves): per 4 taps 4 + 4 loads, 16 MACs. FPU: 4-cycle latency,
 * 1/cycle issue, so the four accumulators keep it busy. x must have taps-1 valid samples before x[0]. */
static void fir_core(const float *restrict h, int taps, const float *restrict x, float *restrict y, int n)
{
    int i = 0, t4 = taps & ~3;
    for (; i + 3 < n; i += 4) {
        const float *p = x + i;
        float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        float q0 = p[0], q1 = p[1], q2 = p[2], q3 = p[3];
        int j = 0;
        for (; j < t4; j += 4) {
            float h0 = h[j], h1 = h[j + 1], h2 = h[j + 2], h3 = h[j + 3];
            a0 += h0 * q0; a1 += h0 * q1; a2 += h0 * q2; a3 += h0 * q3; q3 = p[-j - 1];
            a0 += h1 * q3; a1 += h1 * q0; a2 += h1 * q1; a3 += h1 * q2; q2 = p[-j - 2];
            a0 += h2 * q2; a1 += h2 * q3; a2 += h2 * q0; a3 += h2 * q1; q1 = p[-j - 3];
            a0 += h3 * q1; a1 += h3 * q2; a2 += h3 * q3; a3 += h3 * q0; q0 = p[-j - 4];
        }
        for (; j < taps; j++) { float hj = h[j]; a0 += hj * p[-j]; a1 += hj * p[1 - j]; a2 += hj * p[2 - j]; a3 += hj * p[3 - j]; }
        y[i] = a0; y[i + 1] = a1; y[i + 2] = a2; y[i + 3] = a3;
    }
    for (; i < n; i++) { float a = 0; for (int j = 0; j < taps; j++) a += h[j] * x[i - j]; y[i] = a; }
}

void dsp_fir_f32_init(dsp_fir_f32_t *f, const float *h, float *delay, int taps)
{
    f->h = h; f->delay = delay; f->taps = taps;
    memset(delay, 0, sizeof(float) * 2 * taps);
}

/* delay holds 2*taps floats: [taps-1 history][up to taps new samples] staging for the first outputs */
void dsp_fir_f32(dsp_fir_f32_t *f, const float *in, float *out, int n)
{
    int t = f->taps, head = n < t - 1 ? n : t - 1;
    float *d = f->delay;                                            /* d[0..t-2] = last t-1 inputs */
    memcpy(d + t - 1, in, sizeof(float) * head);
    fir_core(f->h, t, d + t - 1, out, head);                        /* outputs that reach into the history */
    if (n > head) fir_core(f->h, t, in + head, out + head, n - head);
    if (n >= t - 1) memcpy(d, in + n - (t - 1), sizeof(float) * (t - 1));
    else { memmove(d, d + n, sizeof(float) * (t - 1)); }
}

/* DF2 transposed biquad: coef = b0 b1 b2 a1 a2 (a0 = 1), w = 2 state words (ESP-DSP dsps_biquad_f32 layout) */
void dsp_biquad_f32(const float *restrict x, float *restrict y, int n, const float *restrict c, float *restrict w)
{
#if SOC_DSP_F32_ASM
    dsp_biquad_f32_asm(x, y, n & ~3, c, w);                         /* n % 4 tail below */
    x += n & ~3; y += n & ~3; n &= 3;
#endif
    float b0 = c[0], b1 = c[1], b2 = c[2], a1 = c[3], a2 = c[4], w0 = w[0], w1 = w[1];
    for (int i = 0; i < n; i++) {                                   /* direct form II, as ESP-DSP */
        float t = x[i] - a2 * w1, u = b1 * w0 + b2 * w1;             /* off the w0 -> d0 critical path */
        float d0 = t - a1 * w0;
        y[i] = u + b0 * d0;
        w1 = w0; w0 = d0;
    }
    w[0] = w0; w[1] = w1;
}

__attribute__((optimize("unroll-loops"))) void dsp_add_f32(const float *restrict a, const float *restrict b, float *restrict y, int n)
{
    int i = 0;
#if SOC_DSP_F32_ASM
    if (al3(a, b, y)) { dsp_vadd8_f32(a, b, y, n >> 3); i = n & ~7; }
#endif
    for (; i < n; i++) y[i] = a[i] + b[i];
}
__attribute__((optimize("unroll-loops"))) void dsp_sub_f32(const float *restrict a, const float *restrict b, float *restrict y, int n)
{
    int i = 0;
#if SOC_DSP_F32_ASM
    if (al3(a, b, y)) { dsp_vsub8_f32(a, b, y, n >> 3); i = n & ~7; }
#endif
    for (; i < n; i++) y[i] = a[i] - b[i];
}
__attribute__((optimize("unroll-loops"))) void dsp_mul_f32(const float *restrict a, const float *restrict b, float *restrict y, int n)
{
    int i = 0;
#if SOC_DSP_F32_ASM
    if (al3(a, b, y)) { dsp_vmul8_f32(a, b, y, n >> 3); i = n & ~7; }
#endif
    for (; i < n; i++) y[i] = a[i] * b[i];
}
__attribute__((optimize("unroll-loops"))) void dsp_mulc_f32(const float *restrict a, float c, float *restrict y, int n)
{
    int i = 0;
#if SOC_DSP_F32_ASM
    if (al3(a, y, 0)) { union { float f; uint32_t u; } cb = { c }; dsp_vmulc8_f32(a, y, n >> 3, cb.u); i = n & ~7; }
#endif
    for (; i < n; i++) y[i] = a[i] * c;
}
__attribute__((optimize("unroll-loops"))) void dsp_addc_f32(const float *restrict a, float c, float *restrict y, int n) { for (int i = 0; i < n; i++) y[i] = a[i] + c; }
__attribute__((optimize("unroll-loops"))) void dsp_mac_f32(const float *restrict a, const float *restrict b, float *restrict y, int n) { for (int i = 0; i < n; i++) y[i] += a[i] * b[i]; }
__attribute__((optimize("unroll-loops"))) void dsp_abs_f32(const float *restrict a, float *restrict y, int n) { for (int i = 0; i < n; i++) y[i] = __builtin_fabsf(a[i]); }
__attribute__((optimize("unroll-loops"))) void dsp_clip_f32(const float *restrict a, float lo, float hi, float *restrict y, int n) { for (int i = 0; i < n; i++) y[i] = a[i] < lo ? lo : a[i] > hi ? hi : a[i]; }
float dsp_sum_f32(const float *a, int n)
{
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0; int i = 0;
    for (; i + 3 < n; i += 4) { s0 += a[i]; s1 += a[i + 1]; s2 += a[i + 2]; s3 += a[i + 3]; }
    for (; i < n; i++) s0 += a[i];
    return (s0 + s1) + (s2 + s3);
}
void dsp_minmax_f32(const float *a, int n, float *mn, float *mx, int *imn, int *imx)
{
    float lo = a[0], hi = a[0]; int il = 0, ih = 0;
    for (int i = 1; i < n; i++) { if (a[i] < lo) { lo = a[i]; il = i; } if (a[i] > hi) { hi = a[i]; ih = i; } }
    if (mn) *mn = lo;
    if (mx) *mx = hi;
    if (imn) *imn = il;
    if (imx) *imx = ih;
}

/* C[m][k] = A[m][n] * B[n][k], row-major; 4x2 register tiles (8 accumulators, 6 loads per 8 MACs) */
void dsp_mat_mul_f32(const float *restrict A, const float *restrict B, float *restrict C, int m, int n, int k)
{
#if SOC_DSP_F32_ASM
    if (m > 0 && !(n & 3) && !(k & 3) && al3(A, B, C)) { dsp_mm4_f32(A, B, C, m, n, k); return; }
#endif
    int i = 0;
    for (; i + 3 < m; i += 4) {
        int j = 0;
        for (; j + 1 < k; j += 2) {
            float c00 = 0, c01 = 0, c10 = 0, c11 = 0, c20 = 0, c21 = 0, c30 = 0, c31 = 0;
            const float *a = A + i * n, *b = B + j;
#pragma GCC unroll 2
            for (int p = 0; p < n; p++, b += k) {
                float b0 = b[0], b1 = b[1], a0 = a[p], a1 = a[n + p], a2 = a[2 * n + p], a3 = a[3 * n + p];
                c00 += a0 * b0; c01 += a0 * b1; c10 += a1 * b0; c11 += a1 * b1;
                c20 += a2 * b0; c21 += a2 * b1; c30 += a3 * b0; c31 += a3 * b1;
            }
            float *c = C + i * k + j;
            c[0] = c00; c[1] = c01; c[k] = c10; c[k + 1] = c11; c[2 * k] = c20; c[2 * k + 1] = c21; c[3 * k] = c30; c[3 * k + 1] = c31;
        }
        for (; j < k; j++) for (int r = 0; r < 4; r++) { float s = 0; for (int p = 0; p < n; p++) s += A[(i + r) * n + p] * B[p * k + j]; C[(i + r) * k + j] = s; }
    }
    for (; i < m; i++) for (int j = 0; j < k; j++) { float s = 0; for (int p = 0; p < n; p++) s += A[i * n + p] * B[p * k + j]; C[i * k + j] = s; }
}
void dsp_mat_add_f32(const float *A, const float *B, float *C, int m, int n) { dsp_add_f32(A, B, C, m * n); }
void dsp_mat_trans_f32(const float *A, float *T, int m, int n) { for (int i = 0; i < m; i++) for (int j = 0; j < n; j++) T[j * m + i] = A[i * n + j]; }

/* full convolution, out length ns + nk - 1 */
void dsp_conv_f32(const float *s, int ns, const float *h, int nk, float *out)
{
    if (ns < nk) { const float *t = s; s = h; h = t; int q = ns; ns = nk; nk = q; }
    for (int i = 0; i < nk - 1; i++) { float a = 0; for (int j = 0; j <= i; j++) a += h[j] * s[i - j]; out[i] = a; }
    fir_core(h, nk, s + nk - 1, out + nk - 1, ns - nk + 1);          /* full-overlap part */
    for (int i = ns; i < ns + nk - 1; i++) { float a = 0; for (int j = i - ns + 1; j < nk; j++) a += h[j] * s[i - j]; out[i] = a; }
}

/* correlation, out[i] = sum_j s[i + j] p[j], i in [0, ns - np] */
void dsp_corr_f32(const float *restrict s, int ns, const float *restrict p, int np, float *restrict out)
{
    int n = ns - np + 1, i = 0;
    if (np <= 256) {                                                 /* = FIR with the reversed pattern */
        float pr[256];
        for (int j = 0; j < np; j++) pr[j] = p[np - 1 - j];
        fir_core(pr, np, s + np - 1, out, n);
        return;
    }
    for (; i + 3 < n; i += 4) {
        const float *q = s + i;
        float a0 = 0, a1 = 0, a2 = 0, a3 = 0, v0 = q[0], v1 = q[1], v2 = q[2], v3;
        for (int j = 0; j < np; j++) {
            v3 = q[j + 3]; float pj = p[j];
            a0 += pj * v0; a1 += pj * v1; a2 += pj * v2; a3 += pj * v3;
            v0 = v1; v1 = v2; v2 = v3;
        }
        out[i] = a0; out[i + 1] = a1; out[i + 2] = a2; out[i + 3] = a3;
    }
    for (; i < n; i++) { float a = 0; for (int j = 0; j < np; j++) a += s[i + j] * p[j]; out[i] = a; }
}

/* moving average of width w (running sum), y[i] = mean(x[i-w+1..i]) with the first w-1 over what exists */
void dsp_movavg_f32(const float *x, float *y, int n, int w)
{
    double s = 0;
    for (int i = 0; i < n; i++) { s += x[i]; if (i >= w) s -= x[i - w]; y[i] = (float)(s / (i + 1 < w ? i + 1 : w)); }
}

/* local maxima above thr, at least dist samples apart (the larger wins); returns count */
int dsp_peaks_f32(const float *x, int n, float thr, int dist, int *idx, int max)
{
    int c = 0;
    for (int i = 1; i + 1 < n; i++) {
        if (x[i] < thr || x[i] < x[i - 1] || x[i] <= x[i + 1]) continue;
        if (c && i - idx[c - 1] < dist) { if (x[i] > x[idx[c - 1]]) idx[c - 1] = i; continue; }
        if (c < max) idx[c++] = i;
    }
    return c;
}

/* ---- int16 (ESP-DSP semantics: truncating shift, saturation as its vector code) ---- */
static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
void dsp_add_s16(const int16_t *a, const int16_t *b, int16_t *y, int n, int shift)
{
    int i = 0;
#if SOC_CPU_HAS_PIE
    if (al3(a, b, y)) { dsp_vadds16_sh(a, b, y, n >> 3, shift); i = n & ~7; }
#endif
    for (; i < n; i++) y[i] = sat16(sat16(a[i] + b[i]) >> shift);
}
void dsp_mul_s16(const int16_t *a, const int16_t *b, int16_t *y, int n, int shift)
{
    int i = 0;
#if SOC_CPU_HAS_PIE
    if (al3(a, b, y)) { dsp_vmuls16_sh(a, b, y, n >> 4, shift); i = n & ~15; }
#endif
    for (; i < n; i++) y[i] = sat16(((int32_t)a[i] * b[i]) >> shift);
}
/* C[m][k] = sat(A[m][n] B[n][k] >> shift) */
void dsp_mat_mul_s16(const int16_t *A, const int16_t *B, int16_t *C, int m, int n, int k, int shift)
{
#if SOC_CPU_HAS_PIE
    if (m > 0 && n > 0 && !(k & 7) && !(((uintptr_t)B | (uintptr_t)C) & 15)) { dsp_mm8_s16(A, B, C, m, n, k, shift); return; }
#endif
    for (int i = 0; i < m; i++) for (int j = 0; j < k; j++) {
        int64_t acc = 0;
        for (int p = 0; p < n; p++) acc += (int32_t)A[i * n + p] * B[p * k + j];
        acc >>= shift; C[i * k + j] = acc > 32767 ? 32767 : acc < -32768 ? -32768 : (int16_t)acc;
    }
}

/* decimating int16 FIR: y[i] = sat((sum_j h[j] x[(i+1) decim - 1 - j] + 2^(shift-1)) >> shift), state across calls.
 * Works on a linear window [taps-1 history | input] in f->work; one PIE dot per output against the reversed,
 * zero-padded coefficients. */
int dsp_fird_s16_init(dsp_fird_s16_t *f, const int16_t *h, int taps, int decim, int shift, int16_t *hr, int16_t *work, int max_in)
{
    if (taps < 1 || decim < 1 || ((uintptr_t)hr & 15) || ((uintptr_t)work & 15)) return -1;
    int t8 = (taps + 7) & ~7;
    for (int q = 0; q < t8; q++) hr[q] = q < taps ? h[taps - 1 - q] : 0;
    *f = (dsp_fird_s16_t){ hr, work, taps, t8, decim, shift, max_in };
    memset(work, 0, sizeof(int16_t) * (taps - 1));
    return 0;
}

int dsp_fird_s16(dsp_fird_s16_t *f, const int16_t *in, int16_t *out, int n_out)
{
    int nin = n_out * f->decim, h1 = f->taps - 1;
    if (nin > f->max_in) return -1;
    int16_t *w = f->work;
    memcpy(w + h1, in, sizeof(int16_t) * nin);
    int32_t rnd = f->shift ? 1 << (f->shift - 1) : 0;
#if SOC_CPU_HAS_PIE
    dsp_fird_core_s16(w + f->decim - 1, f->hr, out, n_out, 2 * f->decim, f->t8 >> 3, f->shift, rnd);
    memmove(w, w + nin, sizeof(int16_t) * h1);
    return n_out;
#endif
    for (int i = 0; i < n_out; i++) {
        const int16_t *x = w + (i + 1) * f->decim - 1;          /* window start: x[(i+1)d - 1 - (taps-1)] */
        int32_t acc;
#if SOC_CPU_HAS_PIE
        acc = dsp_dotu_s16(x, f->hr, f->t8 >> 3);
#else
        acc = 0; for (int q = 0; q < f->taps; q++) acc += (int32_t)x[q] * f->hr[q];
#endif
        out[i] = sat16((acc + rnd) >> f->shift);
    }
    memmove(w, w + nin, sizeof(int16_t) * h1);                    /* keep the last taps-1 inputs */
    return n_out;
}
