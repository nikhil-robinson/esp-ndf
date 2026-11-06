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

