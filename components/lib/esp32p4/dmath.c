/* floor / ceil / trunc / round / rint on the bit pattern. The musl versions add and subtract 2^52 in double: four soft-float
 * operations (350 cycles) for what is a mask and an increment here. */
#include <stdint.h>

typedef union { double d; uint64_t u; } du_t;
#define SIGN 0x8000000000000000ull
#define ONE  0x3FF0000000000000ull

static inline double mk(uint64_t u) { du_t v; v.u = u; return v.d; }
static inline uint64_t bits(double d) { du_t v; v.d = d; return v.u; }

double floor(double x)
{
    uint64_t u = bits(x);
    unsigned e = u >> 52 & 0x7FF;
    if (e >= 0x433 || !(u << 1)) return x;                      /* integral, zero, inf, NaN */
    if (e < 0x3FF) return mk(u >> 63 ? SIGN | ONE : 0);        /* |x| < 1: -1 or +0 */
    uint64_t m = (1ull << (0x433 - e)) - 1;
    if (!(u & m)) return x;
    return mk(u >> 63 ? (u + m) & ~m : u & ~m);
}
double ceil(double x)
{
    uint64_t u = bits(x);
    unsigned e = u >> 52 & 0x7FF;
    if (e >= 0x433 || !(u << 1)) return x;
    if (e < 0x3FF) return mk(u >> 63 ? SIGN : ONE);            /* |x| < 1: -0 or +1 */
    uint64_t m = (1ull << (0x433 - e)) - 1;
    if (!(u & m)) return x;
    return mk(u >> 63 ? u & ~m : (u + m) & ~m);
}
double trunc(double x)
{
    uint64_t u = bits(x);
    unsigned e = u >> 52 & 0x7FF;
    if (e >= 0x433) return x;
    if (e < 0x3FF) return mk(u & SIGN);
    return mk(u & ~((1ull << (0x433 - e)) - 1));
}
double round(double x)                                          /* half away from zero */
{
    uint64_t u = bits(x);
    unsigned e = u >> 52 & 0x7FF;
    if (e >= 0x433) return x;
    if (e < 0x3FE) return mk(u & SIGN);
    if (e == 0x3FE) return mk((u & SIGN) | ONE);               /* 0.5 <= |x| < 1 */
    uint64_t m = (1ull << (0x433 - e)) - 1;
    return mk((u + ((m + 1) >> 1)) & ~m);
}
double rint(double x)                                           /* half to even */
{
    uint64_t u = bits(x);
    unsigned e = u >> 52 & 0x7FF;
    if (e >= 0x433) return x;
    if (e < 0x3FE) return mk(u & SIGN);
    if (e == 0x3FE) return mk((u & SIGN) | ((u & ~SIGN) > 0x3FE0000000000000ull ? ONE : 0));   /* > 0.5 rounds up, 0.5 to even (0) */
    uint64_t m = (1ull << (0x433 - e)) - 1, half = (m + 1) >> 1, frac = u & m;
    u &= ~m;
    if (frac > half || (frac == half && (u & (m + 1)))) u += m + 1;
    return mk(u);
}

/* ---- sin / cos kernels (fdlibm k_sin / k_cos): fewer soft-float operations than the musl forms (sin 14 against 17) ---- */
double __sin(double x, double y, int iy)
{
    static const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03, S3 = -1.98412698298579493134e-04,
                        S4 = 2.75573137070700676789e-06, S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
    double z = x * x, v = z * x;
    double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (iy == 0) return x + v * (S1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}
double __cos(double x, double y)
{
    static const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03, C3 = 2.48015872894767294178e-05,
                        C4 = -2.75573143513906633035e-07, C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
    uint64_t u = bits(x);
    uint32_t ix = (uint32_t)(u >> 32) & 0x7FFFFFFFu;
    if (ix < 0x3E400000u && (int)x == 0) return 1.0;                   /* |x| < 2^-27: cos = 1 */
    double z = x * x;
    double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    if (ix < 0x3FD33333u) return 1.0 - (0.5 * z - (z * r - x * y));   /* |x| < 0.3 */
    double qx = ix > 0x3FE90000u ? 0.28125 : mk((uint64_t)(ix - 0x00200000u) << 32);      /* x / 4 truncated: |x| > 0.78125 -> 0.28125 */
    double hz = 0.5 * z - qx, a = 1.0 - qx;
    return a - (hz - (z * r - x * y));
}
