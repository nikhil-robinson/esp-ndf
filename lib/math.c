/* Double math library (C99 <math.h>, double functions). Own implementation on
 * the soft-double arithmetic of lib/double.c / lib/double.S, with integer
 * arithmetic where exactness is cheaper that way. */
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

static inline uint64_t asu(double d) { union { double d; uint64_t u; } v = { d }; return v.u; }
static inline double asd(uint64_t u) { union { double d; uint64_t u; } v = { .u = u }; return v.d; }

static inline uint64_t mul64(uint64_t a, uint64_t b, uint64_t *lo)   /* a * b = hi:lo */
{
    uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    *lo = mid << 32 | (uint32_t)ll;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

float sqrtf(float);

/* Correctly rounded: T = sqrt(m 2^52) for m in [2^52, 2^54), from a float
 * seed and two Newton corrections on the exact residual, then the rounding
 * decided exactly by 4 M vs (2T + 1)^2 (never equal: no ties for sqrt). */
double sqrt(double x)
{
    uint64_t u = asu(x);
    if (u >= 0x7FF0000000000000ull || !u) {           /* negative, +inf, nan, +0 */
        if (u == 0x7FF0000000000000ull || !(u << 1) || (u & ~(1ull << 63)) > 0x7FF0000000000000ull) return x + x;   /* +inf, -0, nan */
        return (x - x) / (x - x);                      /* x < 0 */
    }
    int e = (int)(u >> 52);
    uint64_t m = u & 0xFFFFFFFFFFFFFull;
    if (!e) { int s = __builtin_clzll(m) - 11; m <<= s; e = 1 - s; } else m |= 1ull << 52;
    e -= 1023;                                         /* x = m 2^(e - 52) */
    if (e & 1) { m <<= 1; e--; }                       /* m in [2^52, 2^54), e even */
    /* M = m 2^52 (hi:lo), T ~ sqrt(M) in [2^52, 2^53) */
    uint64_t Mh = m >> 12, Ml = m << 52;
    float sf = sqrtf((float)m);                        /* sqrt(m) ~ 2^26, 24 bits */
    uint64_t T = (uint64_t)(sf * 67108864.0f);         /* * 2^26 */
    float inv = 0.5f / (sf * 67108864.0f);             /* 1 / (2T), 24 bits */
    for (int it = 0; it < 2; it++) {
        uint64_t sl, sh = mul64(T, T, &sl);            /* T^2 */
        /* D = M - T^2 (signed, |D| < 2^90): as a double for the correction */
        bool neg = sh > Mh || (sh == Mh && sl > Ml);
        uint64_t dh, dl;
        if (!neg) { dl = Ml - sl; dh = Mh - sh - (Ml < sl); } else { dl = sl - Ml; dh = sh - Mh - (sl < Ml); }
        float d = (float)dh * 18446744073709551616.0f + (float)dl;   /* |D| to 24 bits */
        int64_t c = (int64_t)(d * inv);
        T = neg ? T - (uint64_t)c : T + (uint64_t)c;
    }
    /* exact: largest T with T^2 <= M, then round: T + 1 if 4M > (2T + 1)^2 */
    for (;;) {
        uint64_t sl, sh = mul64(T, T, &sl);
        if (sh > Mh || (sh == Mh && sl > Ml)) { T--; continue; }
        uint64_t T1 = T + 1, s1l, s1h = mul64(T1, T1, &s1l);
        if (s1h < Mh || (s1h == Mh && s1l <= Ml)) { T = T1; continue; }
        break;
    }
    /* (2T+1)^2 = 4T^2 + 4T + 1 vs 4M  <=>  T^2 + T vs M (strict: never equal + 1/4) */
    uint64_t sl, sh = mul64(T, T, &sl);
    uint64_t al = sl + T; uint64_t ah = sh + (al < sl);
    if (ah < Mh || (ah == Mh && al < Ml)) T++;
    /* T in [2^52, 2^53] at exponent e/2 */
    return asd(((uint64_t)(e / 2 + 1023 - 1) << 52) + T);
}

int ndf_signgam;                                       /* sign of the last lgamma (musl's __signgam) */

int *__signgam(void) { return &ndf_signgam; }          /* newlib's math.h: #define signgam (*__signgam()) */

/* round to nearest is the only rounding mode here */
double nearbyint(double x) { return rint(x); }
long lrint(double x) { return (long)rint(x); }
long long llrint(double x) { return (long long)rint(x); }
