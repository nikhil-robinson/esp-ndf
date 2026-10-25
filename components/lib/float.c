/* Float runtime helpers the compiler calls on this FPU (single precision:
 * add/mul/madd are instructions; divide, sqrt and every conversion are
 * calls). libgcc cannot be linked (windowed ABI), so these are ours.
 * Divide and sqrt: components/lib/fpu.S on the FPU's divide/sqrt helper instructions;
 * without an FPU, the portable versions below (exact integer long division,
 * Newton + residual choice). All checked bit-exact against host IEEE results
 * (components/lib/test_apps/libc_test).
 * Double: only conversions (float <-> double, so printf("%f", x) and
 * double constants assigned to floats work); no double arithmetic.
 * ponytail: double add/mul/div would need a soft-float library; use float. */
#include <stdint.h>
#include "soc_caps.h"

typedef union { float f; uint32_t u; } fu_t;
typedef union { double d; uint64_t u; } du_t;

static inline float fma_(float a, float b, float c) { return a * b + c; }   /* contracted to madd.s */

/* Portable versions (always built so components/lib/test_apps/libc_test checks them too). */
/* Exact: integer long division of the 24-bit mantissas (4 x 7-bit steps on
 * the 32-bit divider) gives a 26+ bit quotient and the exact remainder, so
 * every result, denormals included, rounds to nearest-even exactly. */
float ndf_divsf3_soft(float a, float b)
{
    fu_t ua = { a }, ub = { b };
    uint32_t sign = (ua.u ^ ub.u) & 0x80000000u;
    int ea = (int)((ua.u >> 23) & 0xFF), eb = (int)((ub.u >> 23) & 0xFF);
    uint32_t ma = ua.u & 0x7FFFFF, mb = ub.u & 0x7FFFFF;
    fu_t o;
    if (ea == 0xFF || eb == 0xFF) {
        if ((ea == 0xFF && ma) || (eb == 0xFF && mb) || (ea == 0xFF && eb == 0xFF)) { o.u = 0x7FC00000; return o.f; }
        o.u = sign | (ea == 0xFF ? 0x7F800000 : 0); return o.f;
    }
    if (!eb && !mb) { o.u = (!ea && !ma) ? 0x7FC00000 : sign | 0x7F800000; return o.f; }
    if (!ea && !ma) { o.u = sign; return o.f; }
    /* normalise: mantissas with the leading 1 at bit 23 */
    if (ea) ma |= 0x800000; else { ea = 1; while (!(ma & 0x800000)) { ma <<= 1; ea--; } }
    if (eb) mb |= 0x800000; else { eb = 1; while (!(mb & 0x800000)) { mb <<= 1; eb--; } }
    int e = ea - eb + 127;                       /* exponent if ma >= mb */
    if (ma < mb) { ma <<= 1; e--; }              /* now ma/mb in [1, 2) */
    uint32_t q = 1, r = ma - mb;                 /* leading quotient bit; r < mb < 2^24 */
    for (int i = 0; i < 4; i++) {                /* 28 more bits, 7 per 32-bit divide */
        r <<= 7;
        uint32_t d = r / mb;
        q = q << 7 | d;
        r -= d * mb;
    }                                            /* q: 29 bits, value q / 2^28 in [1, 2) */
    int sh = 5;                                  /* bits to drop for a 24-bit mantissa */
    if (e >= 255) { o.u = sign | 0x7F800000; return o.f; }
    if (e <= 0) { sh += 1 - e; e = 1; if (sh >= 30) { o.u = sign; return o.f; } }
    uint32_t m = q >> sh, lost = q & ((1u << sh) - 1), half = 1u << (sh - 1);
    if (lost > half || (lost == half && (r || (m & 1)))) m++;
    if (sh > 5) o.u = sign | m;                  /* denormal (a carry to 2^23 is the smallest normal) */
    else o.u = sign | (((uint32_t)(e - 1) << 23) + m);   /* implicit 1 adds the exponent's last 1; carry -> next binade / inf */
    return o.f;
}

float ndf_sqrtf_soft(float x)
{
    fu_t v = { x };
    if (v.u >= 0x80000000u) { if (v.u == 0x80000000u) return x; fu_t n = { .u = 0x7FC00000 }; return n.f; }
    if (v.u >= 0x7F800000u || v.u == 0) return x;                   /* +inf, nan, +0 */
    int e = (int)(v.u >> 23), sc = 0;
    if (!e) { x *= 16777216.0f; v.f = x; e = (int)(v.u >> 23); sc = -12; }   /* denormal: scale by 2^24 */
    e -= 127;
    int odd = e & 1;
    fu_t m = { .u = (v.u & 0x7FFFFF) | ((uint32_t)(127 + odd) << 23) };    /* m in [1, 4) */
    float y = 1.1f - 0.11f * m.f;                                       /* crude, refined below */
    for (int i = 0; i < 5; i++) y = y * fma_(-0.5f * m.f * y, y, 1.5f);
    float s = m.f * y;
    float r = fma_(-s, s, m.f);
    s = fma_(r, 0.5f * y, s);
    r = fma_(-s, s, m.f);                                           /* final rounding fix-up by the residual sign */
    fu_t su = { s };
    fu_t up = { .u = su.u + 1 }, dn = { .u = su.u - 1 };
    float ru = fma_(-up.f, up.f, m.f), rd = fma_(-dn.f, dn.f, m.f);
    float ar = r < 0 ? -r : r, aru = ru < 0 ? -ru : ru, ard = rd < 0 ? -rd : rd;
    if (aru < ar) { su = up; ar = aru; }
    if (ard < ar) su = dn;
    su.u += (uint32_t)(((e - odd) / 2 + sc)) << 23;
    return su.f;
}

#if !SOC_CPU_HAS_FPU   /* with an FPU: components/lib/fpu.S */
float __divsf3(float a, float b) { return ndf_divsf3_soft(a, b); }
float sqrtf(float x) { return ndf_sqrtf_soft(x); }
#endif

/* ---- conversions -------------------------------------------------------- */
double __extendsfdf2(float f)
{
    fu_t v = { f }; du_t d;
    uint64_t s = (uint64_t)(v.u >> 31) << 63;
    uint32_t e = (v.u >> 23) & 0xFF, m = v.u & 0x7FFFFF;
    if (e == 0xFF) d.u = s | 0x7FF0000000000000ull | (uint64_t)m << 29;
    else if (e) d.u = s | (uint64_t)(e - 127 + 1023) << 52 | (uint64_t)m << 29;
    else if (!m) d.u = s;
    else { int k = 0; while (!(m & 0x800000)) { m <<= 1; k++; } d.u = s | (uint64_t)(1 - 127 - k + 1023) << 52 | (uint64_t)(m & 0x7FFFFF) << 29; }
    return d.d;
}

float __truncdfsf2(double x)
{
    du_t d = { x }; fu_t v;
    uint32_t s = (uint32_t)(d.u >> 63) << 31;
    int e = (int)((d.u >> 52) & 0x7FF);
    uint64_t m = d.u & 0xFFFFFFFFFFFFFull;
    if (e == 0x7FF) { v.u = s | 0x7F800000 | (m ? 0x400000 : 0); return v.f; }
    int fe = e - 1023 + 127;
    if (fe >= 0xFF) { v.u = s | 0x7F800000; return v.f; }
    int sh = 29;
    uint64_t mm = m | (e ? 1ull << 52 : 0);
    if (fe <= 0) { sh += 1 - fe; fe = 0; if (sh > 63) { v.u = s; return v.f; } }
    uint64_t keep = mm >> sh, lost = mm & ((1ull << sh) - 1), half = 1ull << (sh - 1);
    if (lost > half || (lost == half && (keep & 1))) keep++;
    if (fe == 0) v.u = s | (uint32_t)keep;                          /* denormal (may round up to normal: fine) */
    else v.u = s | (((uint32_t)fe << 23) + ((uint32_t)keep - 0x800000));   /* carry bumps the exponent */
    return v.f;
}

static float u64_to_f(uint64_t x)
{
    if (!x) return 0.0f;
    int lz = __builtin_clzll(x);
    int e = 63 - lz;                                                /* top bit index */
    uint64_t m = x << lz;                                           /* top bit at 63 */
    uint32_t keep = (uint32_t)(m >> 40), lost = (uint32_t)(m >> 8) & 0xFFFFFFFF;  /* 24 bits + rest */
    uint32_t sticky = (m & 0xFF) != 0;
    if (lost > 0x80000000u || (lost == 0x80000000u && (sticky || (keep & 1)))) keep++;
    fu_t v = { .u = ((uint32_t)(e + 127) << 23) + (keep - 0x800000) };
    return v.f;
}
float __floatundisf(uint64_t x) { return u64_to_f(x); }
float __floatdisf(int64_t x) { return x < 0 ? -u64_to_f(-(uint64_t)x) : u64_to_f((uint64_t)x); }

static uint64_t f_to_u64(float f, int *neg)
{
    fu_t v = { f }; *neg = v.u >> 31;
    int e = (int)((v.u >> 23) & 0xFF) - 127;
    if (e < 0) return 0;
    if (e > 63) return ~0ull;
    uint64_t m = (v.u & 0x7FFFFF) | 0x800000;
    return e >= 23 ? m << (e - 23) : m >> (23 - e);
}
uint64_t __fixunssfdi(float f) { int n; uint64_t u = f_to_u64(f, &n); return n ? 0 : u; }
int64_t __fixsfdi(float f)
{
    int n; uint64_t u = f_to_u64(f, &n);
    if (u > (n ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull)) return n ? INT64_MIN : INT64_MAX;
    return n ? -(int64_t)u : (int64_t)u;
}
