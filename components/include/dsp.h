/* SIMD (PIE vector unit) and FPU kernels for control and signal processing.
 * Vector kernels need 16-byte aligned buffers; lengths as noted. Not for
 * use inside interrupt handlers (the vector unit and FPU are off there).
 * On a SoC without the vector unit (SOC_CPU_HAS_PIE == 0) the same API is
 * served by portable C (components/lib/dsp.c). */
#pragma once
#include "ndf.h"

#define DSP_ALIGN __attribute__((aligned(16)))

int32_t dsp_dot_s16(const int16_t *a, const int16_t *b, int n);          /* n % 16 == 0 */
void    dsp_unpack_iq10(const uint32_t *in, int16_t *i, int16_t *q, int n);   /* RF IQ words -> int16 I, raw Q; n % 8 == 0, aligned */
void    dsp_dot2_s16(const int16_t *a, const int16_t *b, const int16_t *h, int n, int32_t out[2]);   /* a.h, b.h; n % 16 == 0 */
void    dsp_add_s16_sat(int16_t *dst, const int16_t *a, const int16_t *b, int n);  /* n % 16 == 0 */
void    dsp_copy128(void *dst, const void *src, size_t n);                /* n % 32 == 0 */
float   dsp_dot_f32(const float *a, const float *b, int n);              /* n % 4 == 0 */
void    dsp_copy128u(void *dst, const void *src, size_t n);               /* dst aligned, n % 48 == 0 */
void    dsp_set128(void *dst, const void *pattern16, size_t n);           /* n % 32 == 0 */

/* ---- signal processing library (components/lib/dsp_fft.c, components/lib/dsp_filt.c; ESP-DSP equivalents). Any alignment. ---- */
/* FFT: complex interleaved (re, im), in place, natural order, n a power of 2 up to 4096, unnormalised forward;
 * ifft scales by 1/n. rfft: n real samples -> x[0] = X0, x[1] = X(n/2), x[2k], x[2k+1] = Xk for 0 < k < n/2. */
int  dsp_fft_f32(float *x, int n);
int  dsp_ifft_f32(float *x, int n);
int  dsp_rfft_f32(float *x, int n);
int  dsp_fft_s16(int16_t *x, int n);                  /* complex int16, Q15 twiddles, output = DFT / n */
void dsp_win_hann_f32(float *w, int n);
void dsp_win_blackman_f32(float *w, int n);
void dsp_win_flattop_f32(float *w, int n);
int  dsp_dct_f32(float *x, float *work, int n);       /* DCT-II, unnormalised; work: 2n floats */
typedef struct { const float *h; float *delay; int taps; } dsp_fir_f32_t;
void dsp_fir_f32_init(dsp_fir_f32_t *f, const float *h, float *delay, int taps);   /* delay: 2 * taps floats */
void dsp_fir_f32(dsp_fir_f32_t *f, const float *in, float *out, int n);
void dsp_biquad_f32(const float *x, float *y, int n, const float *coef, float *w);  /* b0 b1 b2 a1 a2; w[2] */
void dsp_add_f32(const float *a, const float *b, float *y, int n);
void dsp_sub_f32(const float *a, const float *b, float *y, int n);
void dsp_mul_f32(const float *a, const float *b, float *y, int n);
void dsp_mulc_f32(const float *a, float c, float *y, int n);
void dsp_addc_f32(const float *a, float c, float *y, int n);
void dsp_mac_f32(const float *a, const float *b, float *y, int n);   /* y += a * b */
void dsp_abs_f32(const float *a, float *y, int n);
void dsp_clip_f32(const float *a, float lo, float hi, float *y, int n);
float dsp_sum_f32(const float *a, int n);
void dsp_minmax_f32(const float *a, int n, float *mn, float *mx, int *imn, int *imx);
void dsp_mat_mul_f32(const float *A, const float *B, float *C, int m, int n, int k);  /* C[m][k] = A[m][n] B[n][k] */
void dsp_mat_add_f32(const float *A, const float *B, float *C, int m, int n);
void dsp_mat_trans_f32(const float *A, float *T, int m, int n);
void dsp_conv_f32(const float *s, int ns, const float *h, int nk, float *out);     /* out: ns + nk - 1 */
void dsp_corr_f32(const float *s, int ns, const float *p, int np, float *out);     /* out: ns - np + 1 */
void dsp_movavg_f32(const float *x, float *y, int n, int w);
int  dsp_peaks_f32(const float *x, int n, float thr, int dist, int *idx, int max);
void dsp_add_s16(const int16_t *a, const int16_t *b, int16_t *y, int n, int shift);   /* sat(a + b) >> shift */
void dsp_mul_s16(const int16_t *a, const int16_t *b, int16_t *y, int n, int shift);   /* sat(a b >> shift) */
void dsp_mat_mul_s16(const int16_t *A, const int16_t *B, int16_t *C, int m, int n, int k, int shift);
/* decimating int16 FIR (ESP-DSP dsps_fird_s16): hr = 16-aligned (taps+7)&~7 int16, work = 16-aligned
 * taps - 1 + max_in + 16 int16. n_out outputs from n_out * decim inputs per call. */
typedef struct { const int16_t *hr; int16_t *work; int taps, t8, decim, shift, max_in; } dsp_fird_s16_t;
int dsp_fird_s16_init(dsp_fird_s16_t *f, const int16_t *h, int taps, int decim, int shift, int16_t *hr, int16_t *work, int max_in);
int dsp_fird_s16(dsp_fird_s16_t *f, const int16_t *in, int16_t *out, int n_out);
/* linear Kalman filter (components/lib/dsp_kalman.c): caller fills F (n x n), H (m x n), Q (n x n), R (m x m), x, P */
#define DSP_KF_MAX 8
typedef struct { int n, m; float x[DSP_KF_MAX], P[DSP_KF_MAX * DSP_KF_MAX], F[DSP_KF_MAX * DSP_KF_MAX], H[DSP_KF_MAX * DSP_KF_MAX],
                 Q[DSP_KF_MAX * DSP_KF_MAX], R[DSP_KF_MAX * DSP_KF_MAX]; } dsp_kf_t;
void dsp_kf_predict(dsp_kf_t *k);
int  dsp_kf_update(dsp_kf_t *k, const float *z);          /* z: m measurements; -1 if S is singular */
