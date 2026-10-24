/* Fast single-precision math for control loops (lib/fastmath.c): no errno, no huge-argument reduction, stated error
 * (examples/fastmath_test measures it against double precision):
 *   fm_sinf / fm_cosf / fm_sincosf  |x| <= 8192: abs error <= 1.2e-7        fm_atan2f / fm_atanf: <= 3e-7 rad 
 *   fm_expf  x in [-87, 88]: rel error <= 2e-7                              fm_logf  x > 0: abs error <= 2e-7 (|log x| < 1)
 *   fm_rsqrtf / fm_sqrtf (rsqrt0 seed + Newton): rel error <= 2e-7
 * Vector forms process n values (any alignment). Q15 / Q31 helpers: rounding, saturating. */
#pragma once
#include <stdint.h>

float fm_sinf(float x);
float fm_cosf(float x);
void  fm_sincosf(float x, float *s, float *c);
float fm_atanf(float x);
float fm_atan2f(float y, float x);
float fm_expf(float x);
float fm_logf(float x);
float fm_rsqrtf(float x);
float fm_sqrtf(float x);
void  fm_sinf_v(const float *x, float *y, int n);
void  fm_cosf_v(const float *x, float *y, int n);
void  fm_expf_v(const float *x, float *y, int n);

typedef int16_t q15_t;
typedef int32_t q31_t;
static inline q15_t q15_sat(int32_t v) { return (q15_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline q15_t q15_from_f(float f) { float v = f * 32768.0f; return q15_sat((int32_t)(v + (v >= 0 ? 0.5f : -0.5f))); }
static inline float q15_to_f(q15_t q) { return q * (1.0f / 32768.0f); }
static inline q15_t q15_mul(q15_t a, q15_t b) { return q15_sat(((int32_t)a * b + (1 << 14)) >> 15); }
static inline q15_t q15_add(q15_t a, q15_t b) { return q15_sat((int32_t)a + b); }
static inline q31_t q31_sat(int64_t v) { return (q31_t)(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v); }
static inline q31_t q31_mul(q31_t a, q31_t b) { return q31_sat(((int64_t)a * b + (1ll << 30)) >> 31); }
static inline q31_t q31_add(q31_t a, q31_t b) { return q31_sat((int64_t)a + b); }
static inline q31_t q31_from_f(float f) { double v = (double)f * 2147483648.0; return q31_sat((int64_t)(v + (v >= 0 ? 0.5 : -0.5))); }
static inline float q31_to_f(q31_t q) { return (float)q * (1.0f / 2147483648.0f); }
