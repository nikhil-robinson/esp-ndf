/* SIMD (PIE vector unit) and FPU kernels for control and signal processing.
 * Vector kernels need 16-byte aligned buffers; lengths as noted. Not for
 * use inside interrupt handlers (the vector unit and FPU are off there).
 * On a SoC without the vector unit (SOC_CPU_HAS_PIE == 0) the same API is
 * served by portable C (lib/dsp.c). */
#pragma once
#include "ndf.h"

#define DSP_ALIGN __attribute__((aligned(16)))

int32_t dsp_dot_s16(const int16_t *a, const int16_t *b, int n);          /* n % 16 == 0 */
void    dsp_add_s16_sat(int16_t *dst, const int16_t *a, const int16_t *b, int n);  /* n % 16 == 0 */
void    dsp_copy128(void *dst, const void *src, size_t n);                /* n % 32 == 0 */
float   dsp_dot_f32(const float *a, const float *b, int n);              /* n % 4 == 0 */
void    dsp_copy128u(void *dst, const void *src, size_t n);               /* dst aligned, n % 32 == 0 */
void    dsp_set128(void *dst, const void *pattern16, size_t n);           /* n % 32 == 0 */
