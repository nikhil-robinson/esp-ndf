/* The dsp.h functions the P4 vector file (dsp_pie.S) does not carry, as plain C with the results of components/lib/dsp.c
 * (the S3 radio-dump unpacking and the FPU dot product: the compiler already uses the P4 FPU, PIE has no float). */
#include "dsp.h"

float dsp_dot_f32(const float *a, const float *b, int n)
{
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;          /* same 4-way summation order as the S3 kernel */
    for (int i = 0; i + 3 < n; i += 4) { s0 += a[i] * b[i]; s1 += a[i + 1] * b[i + 1]; s2 += a[i + 2] * b[i + 2]; s3 += a[i + 3] * b[i + 3]; }
    return (s0 + s1) + (s2 + s3);
}

void dsp_unpack_iq10(const uint32_t *in, int16_t *i, int16_t *q, int n)
{
    for (int k = 0; k < n; k++) { uint32_t w = in[k]; i[k] = (int16_t)((int32_t)(w << 22) >> 22); q[k] = (int16_t)((int32_t)(w << 12) >> 22); }
}

static inline int16_t s16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

/* the last two stages (half-blocks of 2, then 1 complex) of dsp_fft_s16, with the arithmetic of dsp_fft_s16_stage:
 * top' = sat(x * 32767 >> 16 + t), bot' = sat(x * 32767 >> 16 - t), t = (c m + s m') >> 16 as conj(w) * bottom */
void dsp_fft_s16_last2(int16_t *x, int n, const int16_t *wbr)
{
    for (int n2 = 2, blocks = n / 4; n2 >= 1; n2 /= 2, blocks *= 2)
        for (int j = 0; j < blocks; j++) {
            int32_t c = wbr[2 * j], s = wbr[2 * j + 1];
            int16_t *p = x + 4 * n2 * j;
            for (int i = 0; i < n2; i++) {
                int16_t *a = p + 2 * i, *m = a + 2 * n2;
                int32_t tr = (c * m[0] + s * m[1]) >> 16, ti = (c * m[1] - s * m[0]) >> 16;
                int32_t hr = (a[0] * 32767) >> 16, hi = (a[1] * 32767) >> 16;
                a[0] = s16(hr + tr); a[1] = s16(hi + ti); m[0] = s16(hr - tr); m[1] = s16(hi - ti);
            }
        }
}
