/* Portable versions of the dsp.S kernels, used when the SoC has no vector
 * unit (soc_caps.h: SOC_CPU_HAS_PIE == 0). Same results; plain C, so no
 * alignment requirement. */
#include "dsp.h"
#if !SOC_CPU_HAS_PIE

int32_t dsp_dot_s16(const int16_t *a, const int16_t *b, int n)
{
    int64_t s = 0;                                 /* 40-bit accumulator in the vector unit */
    for (int i = 0; i < n; i++) s += (int32_t)a[i] * b[i];
    return s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : (int32_t)s;
}

void dsp_add_s16_sat(int16_t *d, const int16_t *a, const int16_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        int32_t v = a[i] + b[i];
        d[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
    }
}

float dsp_dot_f32(const float *a, const float *b, int n)
{
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;          /* same 4-way summation order as dsp.S */
    for (int i = 0; i + 3 < n; i += 4) { s0 += a[i] * b[i]; s1 += a[i + 1] * b[i + 1]; s2 += a[i + 2] * b[i + 2]; s3 += a[i + 3] * b[i + 3]; }
    return (s0 + s1) + (s2 + s3);
}

void dsp_copy128(void *dst, const void *src, size_t n)
{
    uint32_t *d = dst; const uint32_t *s = src;
    for (size_t i = 0; i < n / 4; i++) d[i] = s[i];
}

void dsp_copy128u(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst; const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

void dsp_set128(void *dst, const void *pattern16, size_t n)
{
    uint32_t *d = dst; const uint32_t *p = pattern16;
    for (size_t i = 0; i < n / 4; i++) d[i] = p[i & 3];
}

#endif
