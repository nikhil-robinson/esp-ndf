/* Fast float math, see fastmath.h. Classic single-precision schemes (octant reduction + minimax polynomials, as in
 * Cephes): sin/cos via a 3-part Cody-Waite reduction by pi/4, atan with tan(pi/8) / tan(3pi/8) folding, exp as
 * 2^n * poly, log as exponent + poly on [sqrt(1/2), sqrt(2)). */
#include "fastmath.h"
#if __has_include("soc_caps.h")
#include "soc_caps.h"
#endif


typedef union { float f; uint32_t u; int32_t i; } fu_t;
#if defined(__XTENSA__)
static inline uint32_t f2u(float f) { uint32_t u; __asm__("rfr %0, %1" : "=a"(u) : "f"(f)); return u; }   /* no memory trip */
static inline float u2f(uint32_t u) { float f; __asm__("wfr %0, %1" : "=f"(f) : "a"(u)); return f; }
static inline int32_t rndi(float f) { int32_t i; __asm__("round.s %0, %1, 0" : "=a"(i) : "f"(f)); return i; }
#else
static inline uint32_t f2u(float f) { fu_t v = { f }; return v.u; }
static inline float u2f(uint32_t u) { fu_t v; v.u = u; return v.f; }
static inline int32_t rndi(float f) { return (int32_t)(f + (f >= 0 ? 0.5f : -0.5f)); }
#endif
#define FOPI   1.27323954473516f                                   /* 4 / pi */
#define DP1    0.78515625f
#define DP2    2.4187564849853515625e-4f
#define DP3    3.77489497744594108e-8f

static inline float sin_poly(float x, float z) { return ((-1.9515295891e-4f * z + 8.3321608736e-3f) * z - 1.6666654611e-1f) * z * x + x; }
static inline float cos_poly(float z) { return ((2.443315711809948e-5f * z - 1.388731625493765e-3f) * z + 4.166664568298827e-2f) * z * z - 0.5f * z + 1.0f; }

static inline __attribute__((always_inline)) void sincos_i(float x, float *s, float *c)
{
    float ax = x < 0 ? -x : x;
    int j = (int)(ax * FOPI);
    j += j & 1;                                                     /* octant pair */
    float y = (float)j;
    float r = ((ax - y * DP1) - y * DP2) - y * DP3, z = r * r;
    float ps = sin_poly(r, z), pc = cos_poly(z);
    int q = (j >> 1) & 3;                                           /* quadrant */
    float sv = (q & 1) ? pc : ps, cv = (q & 1) ? ps : pc;
    if (q & 2) sv = -sv;
    if ((q + 1) & 2) cv = -cv;
    *s = x < 0 ? -sv : sv; *c = cv;
}
void fm_sincosf(float x, float *s, float *c) { sincos_i(x, s, c); }
float fm_sinf(float x) { float s, c; sincos_i(x, &s, &c); return s; }
float fm_cosf(float x) { float s, c; sincos_i(x, &s, &c); return c; }

float fm_atanf(float x)
{
    float ax = x < 0 ? -x : x, base = 0;
    if (ax > 2.414213562373095f) { base = 1.5707963267948966f; ax = -1.0f / ax; }
    else if (ax > 0.4142135623730950f) { base = 0.7853981633974483f; ax = (ax - 1.0f) / (ax + 1.0f); }
    float z = ax * ax;
    float r = base + (((8.05374449538e-2f * z - 1.38776856032e-1f) * z + 1.99777106478e-1f) * z - 3.33329491539e-1f) * z * ax + ax;
    return x < 0 ? -r : r;
}
float fm_atan2f(float y, float x)
{
    if (x == 0) return y > 0 ? 1.5707963267948966f : y < 0 ? -1.5707963267948966f : 0;
    float a = fm_atanf(y / x);
    if (x > 0) return a;
    return y >= 0 ? a + 3.141592653589793f : a - 3.141592653589793f;
}

static inline __attribute__((always_inline)) float exp_i(float x)
{
    if (x > 88.72f) x = 88.72f;
    if (x < -87.33f) return 0;
    int32_t ni = rndi(x * 1.44269504088896341f);
    float n = (float)ni;
    float r = (x - n * 0.693359375f) - n * -2.12194440e-4f, z = r * r;
    float p = (((((1.9875691500e-4f * r + 1.3981999507e-3f) * r + 8.3334519073e-3f) * r + 4.1665795894e-2f) * r + 1.6666665459e-1f) * r + 5.0000001201e-1f) * z + r + 1.0f;
    return u2f(f2u(p) + ((uint32_t)ni << 23));                      /* * 2^n */
}
float fm_expf(float x) { return exp_i(x); }

float fm_logf(float x)
{
    if (x <= 0) return x == 0 ? -1e30f * 1e30f : 0.0f / 0.0f;
    uint32_t xu = f2u(x);
    int e = (int)((xu >> 23) & 255) - 126;
    float m = u2f((xu & 0x007FFFFF) | 0x3F000000);                  /* mantissa in [0.5, 1) */
    if (m < 0.707106781186547524f) { e -= 1; m = m + m - 1.0f; } else m = m - 1.0f;
    float z = m * m;
    float y = ((((((((7.0376836292e-2f * m - 1.1514610310e-1f) * m + 1.1676998740e-1f) * m - 1.2420140846e-1f) * m
                 + 1.4249322787e-1f) * m - 1.6668057665e-1f) * m + 2.0000714765e-1f) * m - 2.4999993993e-1f) * m
                 + 3.3333331174e-1f) * m * z;
    float fe = (float)e;
    y += -2.12194440e-4f * fe;
    y += -0.5f * z;
    return m + y + 0.693359375f * fe;
}

float fm_rsqrtf(float x)
{
#if defined(__XTENSA__) && SOC_CPU_HAS_FPU
    float r;
    __asm__("rsqrt0.s %0, %1" : "=f"(r) : "f"(x));                 /* ~8-bit seed */
#else
    fu_t v = { x }; v.u = 0x5f3759df - (v.u >> 1); float r = v.f;
#endif
    float h = 0.5f * x;
    r = r * (1.5f - h * r * r);                                     /* Newton x2: 7-bit seed -> 28 bits */
    r = r * (1.5f - h * r * r);
#if !(defined(__XTENSA__) && SOC_CPU_HAS_FPU)
    r = r * (1.5f - h * r * r);                                     /* the magic-number seed is 3.4% off: a third step reaches 2e-7 */
#endif
    return r;
}
float fm_sqrtf(float x) { return x > 0 ? x * fm_rsqrtf(x) : 0; }

/* vector forms: four independent elements per step so the 4-cycle FPU latency is covered */
void fm_sinf_v(const float *restrict x, float *restrict y, int n)
{
    int i = 0; float c0, c1, c2, c3;
    for (; i + 3 < n; i += 4) { sincos_i(x[i], y + i, &c0); sincos_i(x[i + 1], y + i + 1, &c1); sincos_i(x[i + 2], y + i + 2, &c2); sincos_i(x[i + 3], y + i + 3, &c3); }
    for (; i < n; i++) sincos_i(x[i], y + i, &c0);
}
void fm_cosf_v(const float *restrict x, float *restrict y, int n)
{
    int i = 0; float s0, s1, s2, s3;
    for (; i + 3 < n; i += 4) { sincos_i(x[i], &s0, y + i); sincos_i(x[i + 1], &s1, y + i + 1); sincos_i(x[i + 2], &s2, y + i + 2); sincos_i(x[i + 3], &s3, y + i + 3); }
    for (; i < n; i++) sincos_i(x[i], &s0, y + i);
}
void fm_expf_v(const float *restrict x, float *restrict y, int n)
{
    int i = 0;
    for (; i + 3 < n; i += 4) { y[i] = exp_i(x[i]); y[i + 1] = exp_i(x[i + 1]); y[i + 2] = exp_i(x[i + 2]); y[i + 3] = exp_i(x[i + 3]); }
    for (; i < n; i++) y[i] = exp_i(x[i]);
}
