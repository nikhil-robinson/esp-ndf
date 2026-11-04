/* Float math library (C99 <math.h>, float functions). Own implementation:
 * kernels are minimax polynomials from tools/gen_libm.py, evaluated with the
 * FPU's fused multiply-add; reductions keep extra precision as float pairs
 * (hi + lo) where cancellation would otherwise cost accuracy.
 *   exp/exp2/exp10/expm1: table 2^(j/32) (hi + lo), degree-1 kernel
 *   log/log2/log10/log1p: 32-entry table, exact integer reduction
 *   sin/cos/tan: Cody-Waite in float pairs below 2^17 (worst case measured
 *     over every float: 5 bits of margin), Payne-Hanek with 2/pi bits above
 *   pow: log2 and exp2 in float pairs (~46 bits)
 * Accuracy and speed: components/lib/test_apps/libm_test vs mpmath and vs ESP-IDF newlib. */
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "libm_coef.h"

static inline uint32_t asu(float f) { union { float f; uint32_t u; } v = { f }; return v.u; }
static inline float asf(uint32_t u) { union { float f; uint32_t u; } v = { .u = u }; return v.f; }
static inline float fma_(float a, float b, float c) { __asm__("madd.s %0, %1, %2" : "+f"(c) : "f"(a), "f"(b)); return c; }
static inline void two_sum(float a, float b, float *s, float *e) { float t = a + b, bb = t - a; *s = t; *e = (a - (t - bb)) + (b - bb); }
/* Polynomials in Estrin form: independent pairs, then powers of t, so the
 * FPU's fused multiply-adds overlap instead of waiting on one chain */
static inline __attribute__((always_inline)) float poly(const float *c, int n, float t)
{
    float t2 = t * t;
    float p01 = fma_(c[1], t, c[0]);
    if (n == 2) return p01;
    float p23 = n > 3 ? fma_(c[3], t, c[2]) : c[2];
    if (n <= 4) return fma_(p23, t2, p01);
    float t4 = t2 * t2;
    float p45 = n > 5 ? fma_(c[5], t, c[4]) : c[4];
    float p67 = n > 7 ? fma_(c[7], t, c[6]) : n > 6 ? c[6] : 0.0f;
    float q0 = fma_(p23, t2, p01);
    float q1 = n > 6 ? fma_(p67, t2, p45) : p45;
    if (n <= 8) return fma_(q1, t4, q0);
    float p89 = n > 9 ? fma_(c[9], t, c[8]) : c[8];
    float t8 = t4 * t4;
    return fma_(p89, t8, fma_(q1, t4, q0));
}
/* ~1/d to about 1 ulp: FPU seed + two Newton steps (callers correct the
 * quotient with a residual, so the last bit does not matter); d normal */
static inline float recip_(float d)
{
    float r;
    __asm__("recip0.s %0, %1" : "=f"(r) : "f"(d));
    float e = fma_(-d, r, 1.0f); r = fma_(r, e, r);
    e = fma_(-d, r, 1.0f); r = fma_(r, e, r);
    return r;
}
#define INF  asf(0x7F800000)
#define QNAN asf(0x7FC00000)

/* ---- exact operations ------------------------------------------------------ */
float fabsf(float x) { return asf(asu(x) & 0x7FFFFFFF); }
float copysignf(float x, float y) { return asf((asu(x) & 0x7FFFFFFF) | (asu(y) & 0x80000000)); }
float fmaf(float a, float b, float c) { return fma_(a, b, c); }
float nanf(const char *s) { (void)s; return QNAN; }
float fminf(float a, float b) { if (a != a) return b; if (b != b) return a; if (a == b) return asu(a) >> 31 ? a : b; return a < b ? a : b; }
float fmaxf(float a, float b) { if (a != a) return b; if (b != b) return a; if (a == b) return asu(a) >> 31 ? b : a; return a > b ? a : b; }
float fdimf(float a, float b) { if (a != a || b != b) return a + b; return a > b ? a - b : 0.0f; }

float truncf(float x)
{
    uint32_t u = asu(x);
    int e = (int)((u >> 23) & 0xFF) - 127;
    if (e >= 23) return x;                              /* integral, inf, nan */
    if (e < 0) return asf(u & 0x80000000);
    return asf(u & ~(0x7FFFFFu >> e));
}
float floorf(float x)
{
    float t = truncf(x);
    return (t != x && x < 0) ? t - 1.0f : t;
}
float ceilf(float x)
{
    float t = truncf(x);
    return (t != x && x > 0) ? t + 1.0f : t;
}
float roundf(float x)                                  /* half away from zero */
{
    uint32_t u = asu(x);
    int e = (int)((u >> 23) & 0xFF) - 127;
    if (e >= 23) return x;
    if (e < -1) return asf(u & 0x80000000);
    if (e == -1) return copysignf(1.0f, x);            /* 0.5 <= |x| < 1 */
    uint32_t half = 0x400000u >> e, mask = 0x7FFFFFu >> e;
    return asf((u + half) & ~mask);
}
float rintf(float x)                                   /* nearest, ties to even */
{
    uint32_t u = asu(x);
    if (((u >> 23) & 0xFF) >= 150) return x;
    float m = copysignf(8388608.0f, x);                /* 2^23 */
    volatile float t = x + m;                          /* the add rounds to an integer */
    t -= m;
    return copysignf(t, x);
}
float nearbyintf(float x) { return rintf(x); }
long lrintf(float x) { return (long)rintf(x); }
long long llrintf(float x) { return (long long)rintf(x); }
long lroundf(float x) { return (long)roundf(x); }
long long llroundf(float x) { return (long long)roundf(x); }

float modff(float x, float *ip)
{
    float t = truncf(x);
    *ip = t;
    if (isinf(x)) return copysignf(0.0f, x);
    return copysignf(x - t, x);
}

float frexpf(float x, int *e)
{
    uint32_t u = asu(x);
    int be = (int)((u >> 23) & 0xFF);
    if (!be) {
        if (!(u << 1)) { *e = 0; return x; }
        x *= 0x1p25f; u = asu(x); be = (int)((u >> 23) & 0xFF) - 25;
    } else if (be == 0xFF) { *e = 0; return x; }
    *e = be - 126;
    return asf((u & 0x807FFFFF) | 0x3F000000);
}

float scalbnf(float x, int n)
{
    if (n > 127) { x *= 0x1p127f; n -= 127; if (n > 127) { x *= 0x1p127f; n -= 127; if (n > 127) n = 127; } }
    else if (n < -126) { x *= 0x1p-102f; n += 102; if (n < -126) { x *= 0x1p-102f; n += 102; if (n < -126) n = -126; } }
    return x * asf((uint32_t)(n + 127) << 23);           /* one rounding (gradual underflow) */
}
float ldexpf(float x, int n) { return scalbnf(x, n); }
float scalblnf(float x, long n) { return scalbnf(x, n > 1000 ? 1000 : n < -1000 ? -1000 : (int)n); }

int ilogbf(float x)
{
    uint32_t u = asu(x) & 0x7FFFFFFF;
    if (!u) return FP_ILOGB0;
    if (u >= 0x7F800000) return u == 0x7F800000 ? 0x7FFFFFFF : FP_ILOGBNAN;
    if (u < 0x00800000) return -127 - (__builtin_clz(u) - 9);
    return (int)(u >> 23) - 127;
}
float logbf(float x)
{
    uint32_t u = asu(x) & 0x7FFFFFFF;
    if (!u) return -INF;
    if (u >= 0x7F800000) return x * x;
    return (float)ilogbf(x);
}

float nextafterf(float x, float y)
{
    if (x != x || y != y) return x + y;
    if (x == y) return y;
    uint32_t u = asu(x);
    if (!(u << 1)) return asf((asu(y) & 0x80000000) | 1);
    if ((x < y) == !(u >> 31)) u++; else u--;
    return asf(u);
}
float nexttowardf(float x, long double y) { return nextafterf(x, (float)y); }

/* exact remainder of |x| / |y| by binary long division on the significands */
static float fmod_core(float x, float y, uint32_t *quo)
{
    uint32_t ux = asu(x), uy = asu(y), sx = ux & 0x80000000;
    ux &= 0x7FFFFFFF; uy &= 0x7FFFFFFF;
    if (!uy || ux >= 0x7F800000 || uy > 0x7F800000) return (x * y) / (x * y);
    if (ux < uy) { *quo = 0; return x; }
    int ex = (int)(ux >> 23), ey = (int)(uy >> 23);
    uint32_t mx, my;
    if (ex) mx = (ux & 0x7FFFFF) | 0x800000; else { int s = __builtin_clz(ux) - 8; mx = ux << s; ex = 1 - s; }
    if (ey) my = (uy & 0x7FFFFF) | 0x800000; else { int s = __builtin_clz(uy) - 8; my = uy << s; ey = 1 - s; }
    uint32_t q = 0;
    for (; ex > ey; ex--) {
        if (mx >= my) { mx -= my; q++; }
        mx <<= 1; q <<= 1;
    }
    if (mx >= my) { mx -= my; q++; }
    *quo = q;
    if (!mx) return asf(sx);
    int s = __builtin_clz(mx) - 8;
    mx <<= s; ex -= s;
    if (ex > 0) return asf(sx | (uint32_t)ex << 23 | (mx & 0x7FFFFF));
    return asf(sx | mx >> (1 - ex));                    /* denormal: exact */
}
float fmodf(float x, float y) { uint32_t q; return fmod_core(x, y, &q); }

float remquof(float x, float y, int *quo)
{
    uint32_t q = 0;
    float ay = fabsf(y);
    if (y != y || x != x) { *quo = 0; return x + y; }
    float r = fmod_core(fabsf(x), ay, &q);
    if (isinf(x) || !ay) { *quo = 0; return QNAN; }
    /* round the quotient to nearest even: compare 2r with |y| */
    if (r > 0.5f * ay || (r == 0.5f * ay && (q & 1)) || (ay < 0x1p-125f && 2 * r > ay)) { r -= ay; q++; }
    int sq = (asu(x) ^ asu(y)) >> 31 ? -1 : 1;
    *quo = sq * (int)(q & 0x7FFFFFFF);
    return asu(x) >> 31 ? -r : r;
}
float remainderf(float x, float y) { int q; return remquof(x, y, &q); }

/* ---- exp family -------------------------------------------------------------- */
#define LN2_32_HI   CF_LN2_32_HI                       /* ln2/32 = hi + lo */
#define LN2_32_LO   CF_LN2_32_LO
#define INV_LN2_32  CF_INV_LN2_32

/* 2^(k/32) * (1 + p) with k = 32 e + j, scaled with one rounding */
static inline float exp_finish(int32_t k, float p)
{
    int j = k & 31, e = k >> 5;
    float t = kExp2TabHi[j];
    float s = t + fma_(t, p, kExp2TabLo[j]);
    if (e >= -125 && e <= 126) return asf(asu(s) + ((uint32_t)e << 23));
    if (e > 0) return asf(asu(s) + ((uint32_t)(e - 1) << 23)) * 2.0f;        /* may overflow to inf: right */
    return asf(asu(s) + ((uint32_t)(e + 100) << 23)) * 0x1p-100f;          /* one rounding into denormals */
}

/* e^x = (h + l) for |x| <= 88 with k = 32 e + j: 2^e (t + t p + tlo) */
static inline void exp_pair_k(int32_t k, float p, float *h, float *l)
{
    int j = k & 31, e = k >> 5;
    float t = kExp2TabHi[j], sc = asf((uint32_t)(e + 127) << 23);
    float l1 = fma_(t, p, kExp2TabLo[j]), h1 = t + l1;   /* renormalise: h = rounded value, l = its residual */
    *h = h1 * sc;
    *l = ((t - h1) + l1) * sc;
}

static inline float exp_kernel(float r) { return fma_(r * r, fma_(r, kExpF[1], kExpF[0]), r); }

/* k = round(z) for |z| < 2^22, as integer and float */
static inline int32_t rnd(float z, float *kf)
{
    float t = z + 0x1.8p23f;
    *kf = t - 0x1.8p23f;
    return (int32_t)(asu(t) - 0x4B400000);
}

float expf(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    if (ax >= 0x42AE0000) {                            /* |x| >= 87: edges */
        if (ax > 0x7F800000) return x + x;
        if (asu(x) == 0xFF800000) return 0.0f;
        if (x > 89.0f) return x * 0x1p127f * 0x1p127f;        /* inf (closer cases: the scaling below) */
        if (x < -104.0f) return 0x1p-100f * 0x1p-100f;        /* 0 */
    } else if (ax < 0x31800000) return 1.0f + x;      /* |x| < 2^-28 */
    float kf;
    int32_t k = rnd(x * INV_LN2_32, &kf);
    float r = fma_(-kf, LN2_32_HI, x);
    r = fma_(-kf, LN2_32_LO, r);
    return exp_finish(k, exp_kernel(r));
}

float exp2f(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    if (ax >= 0x42FC0000) {                            /* |x| >= 126 */
        if (ax > 0x7F800000) return x + x;
        if (asu(x) == 0xFF800000) return 0.0f;
        if (x >= 128.0f) return x * 0x1p127f * 0x1p127f;
        if (x <= -150.0f) return 0x1p-100f * 0x1p-100f;
    } else if (ax < 0x31800000) return 1.0f + x;
    float kf;
    int32_t k = rnd(x * 32.0f, &kf);
    float f = fma_(kf, -0.03125f, x);                  /* exact: |f| <= 1/64 */
    float r = f * CF_LN2_HI;                           /* f ln2 as a pair */
    float rl = fma_(f, CF_LN2_HI, -r) + f * CF_LN2_LO;
    return exp_finish(k, exp_kernel(r) + rl);
}

float exp10f(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    if (ax >= 0x421A209B) {                            /* |x| >= 38.53 */
        if (ax > 0x7F800000) return x + x;
        if (asu(x) == 0xFF800000) return 0.0f;
        if (x > 38.6f) return x * 0x1p127f * 0x1p127f;
        if (x < -45.2f) return 0x1p-100f * 0x1p-100f;
    } else if (ax < 0x31800000) return 1.0f + x * CF_LN10_HI;
    /* 10^x = 2^(x log2 10): k = round(x * 32 log2 10), r = x ln10 - k ln2/32 */
    float kf;
    int32_t k = rnd(x * CF_32LOG2_10, &kf);
    float xh = x * CF_LN10_HI, xl = fma_(x, CF_LN10_HI, -xh) + x * CF_LN10_LO;   /* x ln10 as a pair */
    float r = fma_(-kf, LN2_32_HI, xh);
    r = fma_(-kf, LN2_32_LO, r) + xl;
    return exp_finish(k, exp_kernel(r));
}

float expm1f(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    if (ax >= 0x41C80000) {                            /* |x| >= 25 */
        if (ax > 0x7F800000) return x + x;
        if (x < 0) return -1.0f;
        return expf(x) - 1.0f;                         /* exact enough: e^x >= 7e10 */
    }
    if (ax < 0x3EB17218) {                             /* |x| < ln2/2: kernel */
        if (ax < 0x33000000) return x;                 /* |x| < 2^-25 */
        return fma_(x * x, poly(kExpm1F, 6, x), x);
    }
    /* x = k ln2 + r, |r| <= ln2/2: e^x - 1 = 2^k (1 + q) - 1, q = expm1(r) */
    float kf;
    int32_t k = rnd(x * CF_INV_LN2, &kf);
    float r = fma_(-kf, CF_LN2_16_HI, x);
    r = fma_(-kf, CF_LN2_16_LO, r);
    float q = fma_(r * r, poly(kExpm1F, 6, r), r);
    float two_k = asf((uint32_t)(k + 127) << 23);
    if (k < 24) return fma_(two_k, q, two_k - 1.0f);   /* (2^k - 1) exact for k <= 24 */
    return fma_(two_k, q, two_k) - 1.0f;
}

/* ---- log family ---------------------------------------------------------------
 * x = 2^e m, m in [1,2); interval j of m (top 5 bits); for j >= 13 use m/2, e+1.
 * r = m / c - 1 computed exactly in integers (1/c has 9 bits), as a float
 * pair; log x = e ln2 + log c + log1p(r). */
#define LN2_HI CF_LN2_16_HI                            /* 16 bits: e * LN2_HI exact */
#define LN2_LO CF_LN2_16_LO

typedef struct { float hi, lo; int e; } logp_t;      /* log x = e ln2 + hi + lo, hi = log c + r_hi */

static inline bool log_reduce(float x, int *e, int *j, float *rh, float *rl)
{
    uint32_t u = asu(x);
    if (u - 0x00800000 >= 0x7F000000) return false;    /* <= 0, denormal, inf, nan */
    *e = (int)(u >> 23) - 127;
    *j = (int)(u >> 18) & 31;
    uint32_t m = (u & 0x7FFFFF) | 0x800000;
    uint64_t P = (uint64_t)m * kLogInv[*j];
    int32_t ri;
    float sc;
    if (*j >= 13) { (*e)++; ri = (int32_t)(P - (1ull << 32)); sc = 0x1p-32f; }
    else { ri = (int32_t)(P - (1ull << 31)); sc = 0x1p-31f; }
    int32_t a = ri & ~7;                               /* <= 24 significant bits each */
    *rh = (float)a * sc;
    *rl = (float)(ri - a) * sc;
    return true;
}

/* log1p(r) - r for the reduced r */
static inline float log_kernel(float r) { float r2 = r * r; return r2 * fma_(r, fma_(r, fma_(r, kLogF[2], kLogF[1]), kLogF[0]), -0.5f); }

static float log_special(float x)
{
    uint32_t u = asu(x);
    if (!(u << 1)) return -INF;
    if (u == 0x7F800000 || u > 0x7F800000) return x + x;   /* +inf, nan */
    if (u >> 31) return QNAN;                              /* x < 0 */
    return NAN;                                            /* unreachable */
}

/* log x = h + l for normal positive x */
static void log_pair(float x, float *h, float *l)
{
    int e = 0, j = 0; float rh = 0, rl = 0;
    log_reduce(x, &e, &j, &rh, &rl);
    float hi = (float)e * LN2_HI + kLogCHi[j];         /* exact */
    float lo = fma_((float)e, LN2_LO, kLogCLo[j]);
    float a, al;
    two_sum(hi, rh, &a, &al);
    *h = a; *l = al + (log_kernel(rh) + (rl + lo));
}

float logf(float x)
{
    int e, j; float rh, rl;
    if (!log_reduce(x, &e, &j, &rh, &rl)) {
        uint32_t u = asu(x);
        if (!(u >> 31) && u && u < 0x00800000) return logf(x * 0x1p25f) - 25 * CF_LN2_F;   /* denormal: 25 ln2 rounded once, fine at |log| >= 87 */
        return log_special(x);
    }
    float hi = (float)e * LN2_HI + kLogCHi[j];         /* exact */
    float lo = fma_((float)e, LN2_LO, kLogCLo[j]);
    return hi + (rh + (log_kernel(rh) + (rl + lo)));
}

float log2f(float x)
{
    int e, j; float rh, rl;
    if (!log_reduce(x, &e, &j, &rh, &rl)) {
        uint32_t u = asu(x);
        if (!(u >> 31) && u && u < 0x00800000) return log2f(x * 0x1p25f) - 25.0f;
        return log_special(x);
    }
    /* v = log c + log1p(r) as a pair, then e + v / ln2 */
    float vh, vl;
    two_sum(kLogCHi[j], rh, &vh, &vl);                 /* log c + r: not exact in one float */
    vl += log_kernel(rh) + (rl + kLogCLo[j]);
    float ph = vh * CF_INVLN2_HI;
    float pl = fma_(vh, CF_INVLN2_HI, -ph) + fma_(vh, CF_INVLN2_LO, vl * CF_INVLN2_HI);
    return (float)e + (ph + pl);
}

float log10f(float x)
{
    int e, j; float rh, rl;
    if (!log_reduce(x, &e, &j, &rh, &rl)) {
        uint32_t u = asu(x);
        if (!(u >> 31) && u && u < 0x00800000) return log10f(x * 0x1p25f) - CF_25LOG10_2;
        return log_special(x);
    }
    float vh, vl;
    two_sum(kLogCHi[j], rh, &vh, &vl);                 /* log c + r: not exact in one float */
    vl += log_kernel(rh) + (rl + kLogCLo[j]);
    /* e log10(2) + v / ln10, both as pairs */
    float eh = (float)e * CF_LOG10_2_HI;               /* 15 bits: exact product */
    float el = (float)e * CF_LOG10_2_LO;
    float ph = vh * CF_INVLN10_HI;
    float pl = fma_(vh, CF_INVLN10_HI, -ph) + fma_(vh, CF_INVLN10_LO, vl * CF_INVLN10_HI);
    return eh + (ph + (pl + el));
}

float log1pf(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    if (ax < 0x3C800000) {                             /* |x| < 1/64: kernel directly (r = x exact) */
        if (ax < 0x33000000) return x;
        return x + log_kernel(x);
    }
    if (x <= -1.0f) return x == -1.0f ? -INF : QNAN;
    if (ax >= 0x7F800000) return x + x;
    /* 1 + x = uh + ul exactly; log1p(x) = log(uh) + ul / uh */
    float uh, ul, h, l;
    two_sum(1.0f, x, &uh, &ul);
    log_pair(uh, &h, &l);
    return h + (l + ul / uh);
}

/* ---- trig ---------------------------------------------------------------------- */
#define PIO2_1 CF_PIO2_1                               /* pi/2 = PIO2_1 + PIO2_2 + PIO2_3 (77 bits) */
#define PIO2_2 CF_PIO2_2
#define PIO2_3 CF_PIO2_3

/* x = n pi/2 + (rh + rl), |rh + rl| <= pi/4 (a little more for huge x) */
static int rem_pio2f(float x, float *rh, float *rl)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    if (ax < 0x48000000) {                             /* |x| < 2^17: Cody-Waite in float pairs */
        float kf;
        int32_t n = rnd(x * CF_2OVERPI, &kf);
        float h = kf * PIO2_1, e1 = fma_(kf, PIO2_1, -h);  /* k pio2_1 exactly */
        float a = x - h;                                   /* exact (Sterbenz) */
        float p2 = kf * PIO2_2, e2 = fma_(kf, PIO2_2, -p2);
        float b = a - e1, bb = (a - b) - e1;               /* two-sum (|a| >= |e1|) */
        float c = b - p2;
        float cc = ((b - c) - p2) + bb;                    /* |b| vs |p2|: b can be tiny: full two-sum */
        if (fabsf(p2) > fabsf(b)) cc = ((-p2 - c) + b) + bb;
        float lo = cc - e2 - kf * PIO2_3;
        *rh = c + lo;
        *rl = lo - (*rh - c);
        return n;
    }
    if (ax >= 0x7F800000) { *rh = x - x; *rl = 0; return 0; }
    /* Payne-Hanek: x = m 2^e; bits of 2/pi from i0 = max(1, e - 1) on, 96 of them */
    int e = (int)(ax >> 23) - 150;
    uint32_t m = (ax & 0x7FFFFF) | 0x800000;
    int i0 = e - 1 > 1 ? e - 1 : 1;
    int wi = (i0 - 1) >> 5, o = (i0 - 1) & 31;
    uint32_t w0 = kTwoOverPi[wi], w1 = kTwoOverPi[wi + 1], w2 = kTwoOverPi[wi + 2], w3 = kTwoOverPi[wi + 3];
    if (o) { w0 = w0 << o | w1 >> (32 - o); w1 = w1 << o | w2 >> (32 - o); w2 = w2 << o | w3 >> (32 - o); }
    /* P = m * (w0:w1:w2), 120 bits as p3:p2:p1:p0 */
    uint64_t t2 = (uint64_t)m * w2, t1 = (uint64_t)m * w1, t0 = (uint64_t)m * w0;
    uint32_t p0 = (uint32_t)t2;
    uint64_t s = (t2 >> 32) + (uint32_t)t1;
    uint32_t p1 = (uint32_t)s;
    s = (s >> 32) + (t1 >> 32) + (uint32_t)t0;
    uint32_t p2 = (uint32_t)s;
    uint32_t p3 = (uint32_t)((s >> 32) + (t0 >> 32));
    int S = i0 + 95 - e;                               /* binary point position */
    /* integer part bits S, S+1; fraction = 64 bits below S */
    uint64_t hi64 = (uint64_t)p3 << 32 | p2, lo64 = (uint64_t)p1 << 32 | p0;
    int sh = S - 64;                                   /* 30 .. 38: fraction top at bit S-1 */
    uint64_t frac = hi64 << (64 - sh) | lo64 >> sh;
    uint32_t q = (uint32_t)(hi64 >> sh) & 3;
    int64_t f = (int64_t)frac;
    if (f < 0) q++;                                    /* fraction >= 1/2: round the quadrant up */
    /* r = f 2^-64 pi/2 as a float pair */
    float fh = (float)f;
    float fl = (float)(f - (int64_t)fh);
    float h = fh * CF_PIO2_64_HI;                      /* pi/2 * 2^-64 = hi + lo */
    float l = fma_(fh, CF_PIO2_64_HI, -h) + fma_(fh, CF_PIO2_64_LO, fl * CF_PIO2_64_HI);
    *rh = h + l; *rl = l - (*rh - h);
    if (asu(x) >> 31) { *rh = -*rh; *rl = -*rl; q = -q; }
    return (int)q;
}

static inline float sin_k(float r, float rl)
{
    float t = r * r;
    float p = r * t * fma_(t, fma_(t, kSinF[2], kSinF[1]), kSinF[0]);
    return r + (p + rl * fma_(t, -0.5f, 1.0f));
}
static inline float cos_k(float r, float rl)
{
    float t = r * r, hw = 0.5f * t, w = 1.0f - hw;
    float c = t * t * fma_(t, fma_(t, kCosF[2], kCosF[1]), kCosF[0]);
    return w + (((1.0f - w) - hw) + (c - r * rl));
}

float sinf(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    if (ax <= 0x3F490FDA) {                            /* |x| <= pi/4 */
        if (ax < 0x39800000) return x;                 /* |x| < 2^-12: sin x = x within 1/2 ulp */
        return sin_k(x, 0);
    }
    float rh, rl;
    int n = rem_pio2f(x, &rh, &rl);
    switch (n & 3) {
    case 0: return sin_k(rh, rl);
    case 1: return cos_k(rh, rl);
    case 2: return -sin_k(rh, rl);
    default: return -cos_k(rh, rl);
    }
}

float cosf(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    if (ax <= 0x3F490FDA) {
        if (ax < 0x39000000) return 1.0f;              /* |x| < 2^-13 */
        return cos_k(x, 0);
    }
    float rh, rl;
    int n = rem_pio2f(x, &rh, &rl);
    switch (n & 3) {
    case 0: return cos_k(rh, rl);
    case 1: return -sin_k(rh, rl);
    case 2: return -cos_k(rh, rl);
    default: return sin_k(rh, rl);
    }
}

void sincosf(float x, float *s, float *c)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    float rh = x, rl = 0;
    int n = 0;
    if (ax > 0x3F490FDA) n = rem_pio2f(x, &rh, &rl);
    else if (ax < 0x39000000) { *s = x; *c = 1.0f; return; }
    float sv = sin_k(rh, rl), cv = cos_k(rh, rl);
    switch (n & 3) {
    case 0: *s = sv; *c = cv; break;
    case 1: *s = cv; *c = -sv; break;
    case 2: *s = -sv; *c = -cv; break;
    default: *s = -cv; *c = sv; break;
    }
}

/* tan(r + rl), or -1/tan when odd */
static float tan_k(float r, float rl, int odd)
{
    float t = r * r;
    float p = poly(kTanF, 7, t);
    float th = r + fma_(r * t, p, rl * fma_(r, r, 1.0f));  /* tan' = 1 + tan^2 */
    if (!odd) return th;
    /* -1/tan with the residual of th folded in: -1/(th + tl) */
    float tl = fma_(r * t, p, rl * fma_(r, r, 1.0f)) - (th - r);
    float inv = -1.0f / th;
    float err = fma_(inv, th, 1.0f) + inv * tl;          /* 1 + inv (th + tl) */
    return fma_(inv, err, inv);
}

float tanf(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    if (ax <= 0x3F490FDA) {
        if (ax < 0x39800000) return x;
        return tan_k(x, 0, 0);
    }
    float rh, rl;
    int n = rem_pio2f(x, &rh, &rl);
    return tan_k(rh, rl, n & 1);
}

/* ---- inverse trig --------------------------------------------------------------- */
#define PIO2_H CF_PIO2_HI
#define PIO2_L CF_PIO2_LO
#define PIO4_H CF_PIO4_HI
#define PIO4_L CF_PIO4_LO

/* atan(z) on [0, 1] = z + z t A(t) (degree-9 minimax: no argument reduction);
 * z = zh + zl, returned as hi + lo */
static inline void atan01(float zh, float zl, float *h, float *l)
{
    float t = zh * zh;
    *h = zh;
    *l = zh * t * poly(kAtan1F, 10, t) + zl * fma_(-t, 1.0f - t, 1.0f);   /* + zl / (1 + t) */
}

/* atan(y / x) = h + l for x, y >= 0, finite, not both 0: one division */
static void atan_pair(float y, float x, float *h, float *l)
{
    bool swap = y > x;
    float n = swap ? x : y, d = swap ? y : x;          /* ratio <= 1 */
    float zh, zl;
    if (d == 1.0f) { zh = n; zl = 0; }
    else { float inv = recip_(d); zh = n * inv; zl = fma_(-zh, d, n) * inv; }   /* zl corrects zh exactly */
    float ah, al;
    atan01(zh, zl, &ah, &al);
    if (!swap) { *h = ah; *l = al; return; }
    float sh, se;                                     /* pi/2 - atan(x/y) */
    two_sum(PIO2_H, -ah, &sh, &se);
    *h = sh; *l = se + (PIO2_L - al);
}

float atanf(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    if (ax < 0x39800000) return x;                     /* |x| < 2^-12 */
    if (ax >= 0x4C800000) {                            /* |x| >= 2^26: +-pi/2 */
        if (ax > 0x7F800000) return x + x;
        return copysignf(PIO2_H, x);
    }
    float h, l;
    atan_pair(asf(ax), 1.0f, &h, &l);
    float r = h + l;
    return asu(x) >> 31 ? -r : r;
}

float atan2f(float y, float x)
{
    if (x != x || y != y) return x + y;
    uint32_t ux = asu(x), uy = asu(y), ax = ux & 0x7FFFFFFF, ay = uy & 0x7FFFFFFF;
    float r;
    if (!ay) r = (ux >> 31) ? CF_PI_HI : 0.0f;                     /* y = 0: 0 or pi */
    else if (!ax) r = PIO2_H;
    else if (ax == 0x7F800000) {
        if (ay == 0x7F800000) r = (ux >> 31) ? 3 * PIO4_H : PIO4_H;
        else r = (ux >> 31) ? PIO2_H * 2 : 0.0f;
    } else if (ay == 0x7F800000) r = PIO2_H;
    else {
        float fx = asf(ax), fy = asf(ay);
        if (ay > ax + (40u << 23)) r = PIO2_H;                     /* |y/x| > 2^40 */
        else if ((ux >> 31) && ax > ay + (40u << 23)) r = PIO2_H * 2;
        else if (!(ux >> 31) && ax > ay + (40u << 23)) r = fy / fx; /* tiny: y/x */
        else {
            if (ax < 0x01000000 || ay < 0x01000000) { fx *= 0x1p40f; fy *= 0x1p40f; }   /* keep the ratio away from denormals */
            if (ax > 0x7E000000 || ay > 0x7E000000) { fx *= 0x1p-40f; fy *= 0x1p-40f; }
            float h, l, sh, se;
            atan_pair(fy, fx, &h, &l);
            if (ux >> 31) { two_sum(CF_PI_HI, -h, &sh, &se); r = sh + (se + (CF_PI_LO - l)); }   /* pi - atan */
            else r = h + l;
            return (uy >> 31) ? -r : r;
        }
        if (ux >> 31) r = (CF_PI_HI - r) + CF_PI_LO;              /* x < 0: pi - r */
    }
    return (uy >> 31) ? -r : r;
}

static inline float asin_k(float x, float t) { return fma_(x * t, poly(kAsinF, 6, t), x); }

/* s + s z B(z), with s = sqrt(z) carried as a pair */
static float asin_tail(float z, float *lo)
{
    float s = sqrtf(z);
    float sl = fma_(-s, s, z) / (2.0f * s);            /* sqrt(z) = s + sl */
    float t = s * z;
    float p = poly(kAsinF, 6, z);
    float v = s + fma_(t, p, sl);
    *lo = fma_(t, p, sl) - (v - s);
    return v;
}

float asinf(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    if (ax <= 0x3F000000) {                            /* |x| <= 0.5 */
        if (ax < 0x39800000) return x;
        return asin_k(x, x * x);
    }
    if (ax >= 0x3F800000) return ax == 0x3F800000 ? copysignf(PIO2_H, x) : (x - x) / (x - x);
    float z = 0.5f * (1.0f - asf(ax)), vl;               /* exact */
    float v = asin_tail(z, &vl);
    float r = PIO2_H - 2.0f * v + (PIO2_L - 2.0f * vl);
    return asu(x) >> 31 ? -r : r;
}

float acosf(float x)
{
    uint32_t ux = asu(x), ax = ux & 0x7FFFFFFF;
    if (ax <= 0x3F000000) {                            /* pi/2 - asin x */
        if (ax < 0x32800000) return PIO2_H;
        return PIO2_H - (asin_k(x, x * x) - PIO2_L);
    }
    if (ax >= 0x3F800000) {
        if (ax == 0x3F800000) return (ux >> 31) ? 2 * PIO2_H : 0.0f;
        return (x - x) / (x - x);
    }
    float z = 0.5f * (1.0f - asf(ax)), vl;
    float v = asin_tail(z, &vl);
    if (!(ux >> 31)) return 2.0f * v + 2.0f * vl;      /* x > 0.5: 2 asin(sqrt z) */
    return 2 * PIO2_H - 2.0f * v + (2 * PIO2_L - 2.0f * vl);
}

/* ---- pow: x^y = 2^(y log2 x), log2 x and the product as float pairs ---------------- */
/* log2 x = e + (log c + log1p r) / ln2 as hi + lo (|error| ~2^-46 relative) */
static void log2_pair(float x, float *h, float *l)
{
    int e = 0, j = 0; float rh = 0, rl = 0;                /* x is normal and positive here */
    log_reduce(x, &e, &j, &rh, &rl);
    /* v = log c + r + kernel(r), with r = rh + rl exact */
    float r2h = rh * rh, r2l = fma_(rh, rh, -r2h);
    float k = rh * r2h * fma_(rh, fma_(rh, kLogF[2], kLogF[1]), kLogF[0]);   /* r^3 L(r) */
    float hv = -0.5f * r2h;                               /* -r^2/2 */
    float a, al;
    two_sum(kLogCHi[j], rh, &a, &al);
    float b = a + hv, bl = (a - b) + hv;
    float vh = b, vl = bl + al + (rl + (k + (-0.5f * r2l - rh * rl + kLogCLo[j])));
    float ph = vh * CF_INVLN2_HI;
    float pl = fma_(vh, CF_INVLN2_HI, -ph) + fma_(vh, CF_INVLN2_LO, vl * CF_INVLN2_HI);
    float s = (float)e + ph;                              /* exact for |e| <= 2^? (ph < 1): rounding below */
    *l = ((float)e - s) + ph + pl;
    *h = s;
}

static bool is_int(float y) { return truncf(y) == y; }
static bool is_odd(float y) { if (!is_int(y) || fabsf(y) >= 0x1p24f) return false; return ((int32_t)y) & 1; }

float powf(float x, float y)
{
    uint32_t ux = asu(x), uy = asu(y), ax = ux & 0x7FFFFFFF, ay = uy & 0x7FFFFFFF;
    if (!ay) return 1.0f;                              /* x^0 = 1 (even nan) */
    if (ux == 0x3F800000) return 1.0f;                 /* 1^y = 1 (even nan) */
    if (ax > 0x7F800000 || ay > 0x7F800000) return x + y;
    float sign = 1.0f;
    if (ay == 0x7F800000) {                            /* y = +-inf */
        if (ax == 0x3F800000) return 1.0f;             /* (-1)^inf */
        return ((ax > 0x3F800000) == !(uy >> 31)) ? INF : 0.0f;
    }
    if (ux >> 31) {                                    /* x < 0 */
        if (!is_int(y)) return (x - x) / (x - x);
        if (is_odd(y)) sign = -1.0f;
        x = -x; ux = ax;
    }
    if (!ax) return sign * ((uy >> 31) ? INF : 0.0f);
    if (ax == 0x7F800000) return sign * ((uy >> 31) ? 0.0f : INF);
    if (ax < 0x00800000) { x *= 0x1p25f; }               /* denormal x: log2 adds -25 below */
    float lh, ll;
    log2_pair(x, &lh, &ll);
    if (ax < 0x00800000) { float s = lh - 25.0f; ll += (lh - s) - 25.0f; lh = s; }
    /* z = y * log2 x as a pair */
    float zh = y * lh, zl = fma_(y, lh, -zh) + y * ll;
    float z = zh + zl; zl = zl - (z - zh); zh = z;
    if (zh >= 128.0f) return sign * 0x1p127f * 0x1p127f;
    if (zh <= -150.0f) return sign * 0x1p-100f * 0x1p-100f;
    /* 2^z: k = round(32 z), f = z - k/32 (exact) + zl, 2^f = e^(f ln2) */
    float kf;
    int32_t k = rnd(zh * 32.0f, &kf);
    float f = fma_(kf, -0.03125f, zh) + zl;
    float r = f * CF_LN2_HI;
    float rl = fma_(f, CF_LN2_HI, -r) + f * CF_LN2_LO;
    return sign * exp_finish(k, exp_kernel(r) + rl);
}

float sqrtf(float x);                                  /* components/lib/fpu.S */

float cbrtf(float x)
{
    uint32_t u = asu(x), ax = u & 0x7FFFFFFF;
    if (!ax || ax >= 0x7F800000) return x + x;
    float a = asf(ax), sc = 1.0f;
    if (ax < 0x00800000) { a *= 0x1p24f; sc = 0x1p-8f; }  /* denormal */
    /* r ~ a^(-1/3): bit estimate, 3 Newton steps without division */
    float r = asf(0x54A2FA8C - asu(a) / 3);
    for (int i = 0; i < 3; i++) { float r3 = r * r * r; r = r * fma_(-a, r3, 4.0f) * 0.333333343f; }
    float y = a * r * r;                               /* a^(1/3) */
    /* one correction: y += (a - y^3) / (3 y^2), y^3 as a pair */
    float y2 = y * y, y2l = fma_(y, y, -y2);
    float y3 = y2 * y, y3l = fma_(y2, y, -y3) + y2l * y;
    float d = (a - y3) - y3l;
    y = fma_(d, r * r * 0.333333343f, y);
    y *= sc;
    return u >> 31 ? -y : y;
}

float hypotf(float a, float b)
{
    uint32_t ua = asu(a) & 0x7FFFFFFF, ub = asu(b) & 0x7FFFFFFF;
    if (ua == 0x7F800000 || ub == 0x7F800000) return INF;
    if (ua > 0x7F800000 || ub > 0x7F800000) return a + b;
    if (ua < ub) { uint32_t t = ua; ua = ub; ub = t; }
    float x = asf(ua), y = asf(ub), sc = 1.0f;
    if (ua - ub > (25u << 23)) return x + y;           /* y below half an ulp of x */
    if (ua > 0x5F000000) { x *= 0x1p-70f; y *= 0x1p-70f; sc = 0x1p70f; }        /* keep x^2 finite */
    else if (ub < 0x20800000) { x *= 0x1p70f; y *= 0x1p70f; sc = 0x1p-70f; }    /* and y^2 normal */
    /* x^2 + y^2 as a pair (products exact through fma), then sqrt of the pair */
    float xh = x * x, xl = fma_(x, x, -xh), yh = y * y, yl = fma_(y, y, -yh);
    float h, l;
    two_sum(xh, yh, &h, &l); l += xl + yl;
    float r = sqrtf(h);
    r = fma_(fma_(-r, r, h) + l, 0.5f / r, r);
    return r * sc;
}

/* ---- hyperbolic ------------------------------------------------------------------ */
static void exp_pair(float x, float *h, float *l)    /* |x| <= 88 */
{
    float kf;
    int32_t k = rnd(x * INV_LN2_32, &kf);
    float r = fma_(-kf, LN2_32_HI, x);
    r = fma_(-kf, LN2_32_LO, r);
    exp_pair_k(k, exp_kernel(r), h, l);
}

float sinhf(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    float a = asf(ax), r;
    if (ax >= 0x7F800000) return x + x;
    if (ax < 0x39800000) return x;
    if (ax <= 0x3F800000) {                             /* |x| <= 1: kernel */
        float t = a * a;
        r = fma_(a * t, poly(kSinhF, 5, t), a);
    } else if (ax < 0x41100000) {                       /* |x| < 9: (e - 1/e) / 2, e as a pair */
        float h, l, s, se;
        exp_pair(a, &h, &l);
        float ri = 1.0f / h;
        two_sum(h, -ri, &s, &se);
        r = 0.5f * (s + (se + l + ri * ri * l));        /* 1/(h+l) = ri - ri^2 l */
    } else if (ax < 0x42B17218) r = 0.5f * expf(a);       /* < 88.72 */
    else { float e = expf(0.5f * a); r = (0.5f * e) * e; }  /* to 89.4: no early overflow */
    return asu(x) >> 31 ? -r : r;
}

float coshf(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    float a = asf(ax);
    if (ax >= 0x7F800000) return a;
    if (ax < 0x39800000) return 1.0f;
    if (ax < 0x3F317218) {                              /* |x| < ln2: 1 + t^2 / (2 (1 + t)) */
        float t = expm1f(a);
        return 1.0f + t * t / (2.0f * (1.0f + t));
    }
    if (ax < 0x41100000) {
        float h, l, s, se;
        exp_pair(a, &h, &l);
        float ri = 1.0f / h;
        two_sum(h, ri, &s, &se);
        return 0.5f * (s + (se + l - ri * ri * l));
    }
    if (ax < 0x42B17218) return 0.5f * expf(a);
    float e = expf(0.5f * a);
    return (0.5f * e) * e;
}

float tanhf(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    float a = asf(ax), r;
    if (ax > 0x7F800000) return x + x;
    if (ax >= 0x41100000) r = 1.0f;                     /* |x| >= 9: 1 within half an ulp */
    else if (ax < 0x39800000) return x;
    else if (ax < 0x3F0CCCCD) {                         /* |x| < 0.55: kernel */
        float t = a * a;
        r = fma_(a * t, poly(kTanhF, 7, t), a);
    } else {                                            /* 1 - 2 / (e^2x + 1), all as pairs */
        float h, l, dh, dl;
        exp_pair(2.0f * a, &h, &l);
        two_sum(h, 1.0f, &dh, &dl); dl += l;
        float qh = 2.0f / dh, ql = (fma_(-qh, dh, 2.0f) - qh * dl) / dh;
        float s, se;
        two_sum(1.0f, -qh, &s, &se);
        r = s + (se - ql);
    }
    return asu(x) >> 31 ? -r : r;
}

float asinhf(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    float a = asf(ax), r;
    if (ax >= 0x7F800000) return x + x;
    if (ax < 0x39800000) return x;
    if (ax < 0x3F000000) {                              /* |x| < 0.5: kernel */
        float t = a * a;
        r = fma_(a * t, poly(kAsinhF, 7, t), a);
    } else if (ax < 0x45800000) {                       /* < 2^12: log(a + sqrt(a^2 + 1)) with pairs */
        float t = a * a, tl = fma_(a, a, -t), wh, wl;
        two_sum(t, 1.0f, &wh, &wl); wl += tl;
        float sh = sqrtf(wh), sl = (fma_(-sh, sh, wh) + wl) / (2.0f * sh);
        float vh, vl, h, l;
        two_sum(a, sh, &vh, &vl); vl += sl;
        log_pair(vh, &h, &l);
        r = h + (l + vl / vh);
    } else {                                            /* log(2a) + 1/(4a^2) */
        float h, l;
        log_pair(a, &h, &l);
        r = (h + CF_LN2_HI) + (l + CF_LN2_LO + 0.25f / (a * a));
    }
    return asu(x) >> 31 ? -r : r;
}

float acoshf(float x)
{
    uint32_t u = asu(x);
    if (u > 0x7F800000) return x + x;
    if (x < 1.0f) return (x - x) / (x - x);
    if (u >= 0x4D800000) return u == 0x7F800000 ? x : logf(x) + CF_LN2_F;
    if (u > 0x40000000) return logf(2.0f * x - 1.0f / (x + sqrtf(x * x - 1.0f)));
    float t = x - 1.0f;                                 /* exact */
    return log1pf(t + sqrtf(2.0f * t + t * t));
}

float atanhf(float x)
{
    uint32_t ax = asu(x) & 0x7FFFFFFF;
    float a = asf(ax), r;
    if (ax > 0x3F800000) return ax > 0x7F800000 ? x + x : (x - x) / (x - x);
    if (ax == 0x3F800000) return x / 0.0f;
    if (ax < 0x39800000) return x;
    if (ax < 0x3F000000) {                              /* |x| < 0.5: kernel */
        float t = a * a;
        r = fma_(a * t, poly(kAtanhF, 7, t), a);
    } else {                                            /* log((1 + a) / (1 - a)) / 2, 1 - a exact */
        float nh, nl, d = 1.0f - a, h, l;
        two_sum(1.0f, a, &nh, &nl);
        float qh = nh / d, ql = (fma_(-qh, d, nh) + nl) / d;
        log_pair(qh, &h, &l);
        r = 0.5f * (h + (l + ql / qh));
    }
    return asu(x) >> 31 ? -r : r;
}
