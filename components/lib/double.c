/* IEEE 754 double arithmetic in software (the FPU is single precision):
 * add, sub, mul, div, compares and integer conversions, round to nearest
 * even, denormals, infinities and NaN handled. libgcc cannot be linked
 * (windowed ABI), so these are the helpers the compiler calls. On Xtensa the
 * add/sub/mul fast paths are in components/lib/double.S. Checked bit-exact against host
 * IEEE results in components/lib/test_apps/libc_test. */
#include <stdint.h>
#include "soc_caps.h"

typedef union { double d; uint64_t u; } du_t;
#define SIGN  0x8000000000000000ull
#define EXPM  0x7FF0000000000000ull
#define MANM  0x000FFFFFFFFFFFFFull
#define QNAN  0x7FF8000000000000ull

/* 64-bit shifts by a variable count, inline on the funnel shifter
 * (gcc would call __ashldi3/__lshrdi3). n in 0..63. */
static inline __attribute__((always_inline)) uint64_t shl(uint64_t x, unsigned n)
{
    uint32_t hi = (uint32_t)(x >> 32), lo = (uint32_t)x;
    if (n & 32) { hi = lo << (n & 31); lo = 0; }
#ifdef __XTENSA__
    else __asm__("ssl %2\n\tsrc %0, %0, %1\n\tsll %1, %1" : "+r"(hi), "+r"(lo) : "r"(n));
#else
    else if (n) { hi = hi << n | lo >> (32 - n); lo <<= n; }
#endif
    return (uint64_t)hi << 32 | lo;
}
static inline __attribute__((always_inline)) uint64_t shr(uint64_t x, unsigned n)
{
    uint32_t hi = (uint32_t)(x >> 32), lo = (uint32_t)x;
    if (n & 32) { lo = hi >> (n & 31); hi = 0; }
#ifdef __XTENSA__
    else __asm__("ssr %2\n\tsrc %1, %0, %1\n\tsrl %0, %0" : "+r"(hi), "+r"(lo) : "r"(n));
#else
    else if (n) { lo = lo >> n | hi << (32 - n); hi >>= n; }
#endif
    return (uint64_t)hi << 32 | lo;
}

static inline double mk(uint64_t u) { du_t v; v.u = u; return v.d; }
static inline uint64_t bits(double d) { du_t v; v.d = d; return v.u; }
static inline int isnan_(uint64_t u) { return (u & ~SIGN) > EXPM; }

/* m: value m * 2^(e - 63), m != 0; sticky: nonzero bits below m.
 * Normalises, rounds to 53 bits (nearest even), handles overflow/denormals. */
static double round_pack(uint64_t sign, int e, uint64_t m, int sticky)
{
    int lz = __builtin_clzll(m);
    m = shl(m, lz); e -= lz;                                   /* top bit at 63; value = m * 2^(e-63) */
    int be = e + 1023;                                   /* biased exponent of the leading bit */
    if (be >= 2047) return mk(sign | EXPM);
    int sh = 11;
    if (be <= 0) {                                       /* denormal: lose 1 - be more bits */
        sh += 1 - be;
        if (sh > 64) return mk(sign);                    /* below half the smallest denormal */
    }
    uint64_t keep, rem, half;
    if (sh == 64) { keep = 0; rem = m; half = SIGN; }
    else { keep = shr(m, sh); rem = m - shl(keep, sh); half = shl(1, sh - 1); }
    if (rem > half || (rem == half && (sticky || (keep & 1)))) keep++;
    if (be <= 0) return mk(sign | keep);                 /* carry into bit 52 = smallest normal: right */
    return mk(sign | (((uint64_t)(be - 1) << 52) + keep));   /* implicit bit bumps the exponent field */
}

/* unpack a finite nonzero: returns mantissa with implicit bit, *e = exponent of bit 52 */
static inline uint64_t unpack(uint64_t u, int *e)
{
    int be = (int)((u >> 52) & 0x7FF);
    uint64_t m = u & MANM;
    if (be) { m |= 1ull << 52; *e = be - 1023; }
    else *e = -1022;                                     /* denormal (m != 0 here) */
    return m;
}

static double add_mag(uint64_t a, uint64_t b, int negate_b)
{
    uint64_t sa = a & SIGN, sb = (b & SIGN) ^ (negate_b ? SIGN : 0);
    if (isnan_(a) || isnan_(b)) return mk(QNAN);
    uint64_t ea = a & EXPM, eb = b & EXPM;
    if (ea == EXPM || eb == EXPM) {
        if (ea == EXPM && eb == EXPM && sa != sb) return mk(QNAN);   /* inf - inf */
        return ea == EXPM ? mk(a) : mk((b & ~SIGN) | sb);
    }
    if (!(a & ~SIGN)) return (b & ~SIGN) ? mk((b & ~SIGN) | sb) : mk(sa & sb);   /* -0 + -0 = -0 */
    if (!(b & ~SIGN)) return mk(a);
    int xa, xb;
    uint64_t ma = unpack(a, &xa) << 10, mb = unpack(b, &xb) << 10;   /* bit 62 = leading; 10 guard bits */
    if (xa < xb || (xa == xb && ma < mb)) {              /* |a| >= |b| from here */
        uint64_t t = ma; ma = mb; mb = t; int ti = xa; xa = xb; xb = ti; t = sa; sa = sb; sb = t;
    }
    int d = xa - xb, sticky = 0;
    if (d) {
        if (d > 63) { sticky = 1; mb = 0; }
        else { uint64_t t = shr(mb, d); sticky = shl(t, d) != mb; mb = t; }
    }
    uint64_t m;
    if (sa == sb) m = ma + mb;
    else {
        m = ma - mb;
        if (sticky) m--;                                 /* the lost bits belong below: borrow one */
        if (!m && !sticky) return mk(0);                 /* exact cancellation: +0 */
    }
    return round_pack(sa, xa + 1, m, sticky);            /* bit 62 of m has weight 2^xa: e = xa + 1 */
}

/* ---- fast paths: normal operands, normal result; anything else falls back
 * to the general code above, which is the reference. ---------------------- */
#define IMPL (1ull << 52)
#define NORMAL(e) ((e) - 1u < 2046u)
#define SLOW (~0ull)                                    /* a NaN pattern the fast paths never produce */
static inline uint32_t hi32(uint64_t x) { return (uint32_t)(x >> 32); }
static inline int clz64(uint64_t x) { return hi32(x) ? __builtin_clz(hi32(x)) : 32 + __builtin_clz((uint32_t)x); }
static inline uint64_t mul32(uint32_t a, uint32_t b)          /* 32 x 32 -> 64 on mull/muluh */
{
#ifdef __XTENSA__
    uint32_t lo, hi;
    __asm__("mull %0, %2, %3\n\tmuluh %1, %2, %3" : "=&r"(lo), "=&r"(hi) : "r"(a), "r"(b));
    return (uint64_t)hi << 32 | lo;
#else
    return (uint64_t)a * b;                                   /* mul + mulhu */
#endif
}

#ifndef __XTENSA__
static inline __attribute__((always_inline)) uint64_t add_fast(uint64_t a, uint64_t b)
{
    uint32_t ea = (hi32(a) >> 20) & 0x7FF, eb = (hi32(b) >> 20) & 0x7FF;
    if (!NORMAL(ea) || !NORMAL(eb)) return SLOW;
    if ((a & ~SIGN) < (b & ~SIGN)) { uint64_t t = a; a = b; b = t; uint32_t te = ea; ea = eb; eb = te; }
    uint32_t d = ea - eb;
    if (d > 54) { return a; }                  /* |b| below half an ulp of a */
    uint64_t ma = ((a & MANM) | IMPL) << 9, mb = ((b & MANM) | IMPL) << 9;   /* leading bit 61, 9 guard bits */
    uint64_t t = shr(mb, d);
    uint32_t sticky = shl(t, d) != mb;
    int e = (int)ea;
    uint64_t m;
    if (!((a ^ b) & SIGN)) {
        m = ma + t;
        if (m >> 62) { sticky |= (uint32_t)m & 1; m >>= 1; e++; }
    } else {
        m = ma - t - sticky;                             /* lost bits of b borrow one */
        if (!m) return 0;                                /* exact cancellation: +0 */
        int lz = clz64(m) - 2;
        if (lz > 0) { m = shl(m, lz); e -= lz; }
    }
    uint64_t keep = m >> 9;
    uint32_t rem = (uint32_t)m & 0x1FF;
    if (rem > 0x100 || (rem == 0x100 && (sticky || (keep & 1)))) keep++;
    if (!NORMAL(e)) return SLOW;                         /* denormal or overflow: general path */
    return (a & SIGN) | (((uint64_t)(e - 1) << 52) + keep);   /* a carry to 2^53 bumps the exponent (inf at the top: right) */
}
#endif

#ifdef __XTENSA__                 /* add/sub/mul fast paths: components/lib/double.S, which jumps here otherwise */
double ndf_dadd_slow(double x, double y) { return add_mag(bits(x), bits(y), 0); }
#elif !SOC_CPU_RISCV
double __adddf3(double x, double y)
{
    uint64_t r = add_fast(bits(x), bits(y));
    if (r != SLOW) return mk(r);
    return add_mag(bits(x), bits(y), 0);
}
double __subdf3(double x, double y)
{
    uint64_t r = add_fast(bits(x), bits(y) ^ SIGN);
    if (r != SLOW) return mk(r);
    return add_mag(bits(x), bits(y), 1);
}
#endif
double __negdf2(double a) { return mk(bits(a) ^ SIGN); }

static inline uint64_t mul64hi(uint64_t a, uint64_t b, uint64_t *lo)
{
    uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    *lo = (mid << 32) | (uint32_t)ll;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

#ifndef __XTENSA__
static inline __attribute__((always_inline)) uint64_t mul_fast(uint64_t a, uint64_t b)
{
    uint32_t ea = (hi32(a) >> 20) & 0x7FF, eb = (hi32(b) >> 20) & 0x7FF;
    if (!NORMAL(ea) || !NORMAL(eb)) return SLOW;
    uint64_t ma = ((a & MANM) | IMPL) << 11, mb = ((b & MANM) | IMPL) << 11;   /* leading bits at 63 */
    uint32_t al = (uint32_t)ma, ah = hi32(ma), bl = (uint32_t)mb, bh = hi32(mb);
    /* al and bl have their low 11 bits zero: al*bl < 2^64 contributes only to sticky + carries */
    uint64_t ll = mul32(al, bl), lh = mul32(al, bh), hl = mul32(ah, bl), hh = mul32(ah, bh);
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    uint64_t hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    uint32_t sticky = ((uint32_t)mid | (uint32_t)ll) != 0;
    int e = (int)(ea + eb) - 1022;
    if (!(hi >> 63)) { hi = hi << 1 | (((uint32_t)mid >> 31) & 1); sticky = ((uint32_t)mid << 1 | (uint32_t)ll) != 0; e--; }
    uint64_t keep = hi >> 11;
    uint32_t rem = (uint32_t)hi & 0x7FF;
    if (rem > 0x400 || (rem == 0x400 && (sticky || (keep & 1)))) keep++;
    if (!NORMAL(e)) return SLOW;
    return ((a ^ b) & SIGN) | (((uint64_t)(e - 1) << 52) + keep);
}
#endif

static double mul_general(uint64_t a, uint64_t b)
{
    uint64_t s = (a ^ b) & SIGN;
    if (isnan_(a) || isnan_(b)) return mk(QNAN);
    uint64_t ea = a & EXPM, eb = b & EXPM;
    int za = !(a & ~SIGN), zb = !(b & ~SIGN);
    if (ea == EXPM || eb == EXPM) return (za || zb) ? mk(QNAN) : mk(s | EXPM);
    if (za || zb) return mk(s);
    int xa, xb;
    uint64_t ma = unpack(a, &xa), mb = unpack(b, &xb);
    int la = __builtin_clzll(ma), lb = __builtin_clzll(mb);
    ma = shl(ma, la); xa -= la - 11; mb = shl(mb, lb); xb -= lb - 11;   /* leading bits at 63 (denormals too) */
    uint64_t lo, hi = mul64hi(ma, mb, &lo);              /* hi >= 2^62 */
    if (!(hi >> 63)) { hi = hi << 1 | lo >> 63; lo <<= 1; xa--; }   /* keep the next bit for rounding */
    return round_pack(s, xa + xb + 1, hi, lo != 0);      /* value = hi * 2^(xa+xb-62) */
}

#ifdef __XTENSA__
double ndf_dmul_slow(double x, double y) { return mul_general(bits(x), bits(y)); }
#elif !SOC_CPU_RISCV
double __muldf3(double x, double y)
{
    uint64_t r = mul_fast(bits(x), bits(y));
    return r != SLOW ? mk(r) : mul_general(bits(x), bits(y));
}
#endif

/* 11 quotient bits per step: estimate on the 32-bit divider with the
 * divisor's top 21 bits + 1 (never too big), then at most two corrections. */
static inline __attribute__((always_inline)) uint64_t div_fast(uint64_t a, uint64_t b)
{
    uint32_t ea = (hi32(a) >> 20) & 0x7FF, eb = (hi32(b) >> 20) & 0x7FF;
    if (!NORMAL(ea) || !NORMAL(eb)) return SLOW;
    uint64_t A = (a & MANM) | IMPL, B = (b & MANM) | IMPL;
    int e = (int)ea - (int)eb + 1023;
    if (A < B) { A <<= 1; e--; }                         /* A / B in [1, 2) */
    uint32_t bh1 = hi32(B) + 1, bl = (uint32_t)B, bh = hi32(B);
    uint64_t r = A - B, q = 1;
    for (int i = 0; i < 5; i++) {                        /* 1 + 5 x 11 = 56 quotient bits */
        uint32_t d = (uint32_t)(r >> 21) / bh1;
        r = (r << 11) - (mul32(d, bl) + ((uint64_t)(d * bh) << 32));   /* digit < 2^11, so d * bh fits 32 bits */
        while (r >= B) { r -= B; d++; }
        q = q << 11 | d;
    }
    uint64_t keep = q >> 3;                              /* q: leading bit 55 */
    uint32_t rem = (uint32_t)q & 7;
    if (rem > 4 || (rem == 4 && (r || (keep & 1)))) keep++;
    if (!NORMAL(e)) return SLOW;
    return ((a ^ b) & SIGN) | (((uint64_t)(e - 1) << 52) + keep);
}

static double div_general(uint64_t a, uint64_t b);
double ndf_ddiv_ref(double x, double y) { return div_general(bits(x), bits(y)); }   /* reference for tests */
double ndf_dmul_ref(double x, double y) { return mul_general(bits(x), bits(y)); }
double ndf_dadd_ref(double x, double y) { return add_mag(bits(x), bits(y), 0); }

double __divdf3(double x, double y)
{
    uint64_t a = bits(x), b = bits(y), r0 = div_fast(a, b);
    return r0 != SLOW ? mk(r0) : div_general(a, b);
}

static double div_general(uint64_t a, uint64_t b)
{
    uint64_t s = (a ^ b) & SIGN;
    if (isnan_(a) || isnan_(b)) return mk(QNAN);
    uint64_t ea = a & EXPM, eb = b & EXPM;
    int za = !(a & ~SIGN), zb = !(b & ~SIGN);
    if (ea == EXPM) return eb == EXPM ? mk(QNAN) : mk(s | EXPM);
    if (eb == EXPM) return mk(s);
    if (zb) return za ? mk(QNAN) : mk(s | EXPM);
    if (za) return mk(s);
    int xa, xb;
    uint64_t ma = unpack(a, &xa), mb = unpack(b, &xb);
    int la = __builtin_clzll(ma), lb = __builtin_clzll(mb);
    ma = shl(ma, la - 11); xa -= la - 11; mb = shl(mb, lb - 11); xb -= lb - 11;   /* both [2^52, 2^53) */
    int e = xa - xb;
    if (ma < mb) { ma <<= 1; e--; }                      /* quotient in [1, 2) */
    /* 64 quotient bits, 32-bit divides on 31-bit digits are not enough for a
     * 53-bit divisor: restoring division, 2 bits per step on the 64-bit remainder */
    uint64_t q = 0, r = ma;
    for (int i = 0; i < 62; i++) {
        q <<= 1;
        if (r >= mb) { r -= mb; q |= 1; }
        r <<= 1;
    }
    /* q: 62 bits, leading 1 at bit 61; value q * 2^(e - 61) */
    return round_pack(s, e + 2, q, r != 0);              /* round_pack wants weight of bit 63 */
}


#if SOC_CPU_RISCV
/* ---- RV32: the same arithmetic on 32-bit limbs (the 64-bit helpers above cost a branch and a carry fix-up per shift / compare / add) ---- */
static inline double mk2(uint32_t hi, uint32_t lo) { return mk((uint64_t)hi << 32 | lo); }
static inline int clz32_(uint32_t x) { x &= ~(x >> 1); union { float f; uint32_t u; } c = { .f = (float)x }; return 158 - (int)(c.u >> 23); }   /* x != 0, FPU */

/* add / sub of normal operands with a normal result; 0xFFFFFFFF in *ok = 0 means "take the general path" */
static inline __attribute__((always_inline)) int add32(uint32_t ah, uint32_t al, uint32_t bh, uint32_t bl, uint32_t *rh, uint32_t *rl)
{
    uint32_t ea = ah >> 20 & 0x7FF, eb = bh >> 20 & 0x7FF;
    if (ea - 1u >= 2046u || eb - 1u >= 2046u) return 0;
    if (ea < eb || (ea == eb && ((ah ^ bh) >> 31) && ((ah & 0xFFFFFu) < (bh & 0xFFFFFu) || ((ah & 0xFFFFFu) == (bh & 0xFFFFFu) && al < bl)))) {
        uint32_t t = ah; ah = bh; bh = t; t = al; al = bl; bl = t; t = ea; ea = eb; eb = t;      /* a is the larger magnitude (same sign and equal exponents: either order) */
    }
    uint32_t d = ea - eb;
    if (d > 54) { *rh = ah; *rl = al; return 1; }
    uint32_t sg = ah & 0x80000000u, same = ~(ah ^ bh) >> 31;
    uint32_t mh = (ah & 0xFFFFFu) | 0x100000u, ml = al, nh = (bh & 0xFFFFFu) | 0x100000u, nl = bl;
    mh = mh << 3 | ml >> 29; ml <<= 3; nh = nh << 3 | nl >> 29; nl <<= 3;        /* three guard bits: leading bit 55 */
    uint32_t sticky = 0;
    if (d) {
        if (d < 32) { sticky = nl << (32 - d); nl = nl >> d | nh << (32 - d); nh >>= d; }
        else if (d == 32) { sticky = nl; nl = nh; nh = 0; }
        else if (d < 64) { sticky = nl | nh << (64 - d); nl = nh >> (d - 32); nh = 0; }
        sticky = sticky != 0;
    }
    int e = (int)ea;
    uint32_t rh_, rl_;
    if (same) {
        rl_ = ml + nl; rh_ = mh + nh + (rl_ < ml);
        if (rh_ >> 24) { sticky |= rl_ & 1; rl_ = rl_ >> 1 | rh_ << 31; rh_ >>= 1; e++; }
    } else {
        uint32_t bw = nl + sticky; uint32_t c = bw < nl;                          /* lost bits of b borrow one */
        rl_ = ml - bw; uint32_t b1 = ml < bw;
        rh_ = mh - nh - c - b1;
        if (!(rh_ | rl_)) { *rh = 0; *rl = 0; return 1; }                          /* exact cancellation: +0 */
        int lz = (rh_ ? clz32_(rh_) : 32 + clz32_(rl_)) - 8;
        if (lz > 0) {
            if (lz >= 32) { rh_ = rl_ << (lz - 32); rl_ = 0; } else { rh_ = rh_ << lz | rl_ >> (32 - lz); rl_ <<= lz; }
            e -= lz;
        }
    }
    uint32_t kh = rh_ >> 3, kl = rl_ >> 3 | rh_ << 29, rem = rl_ & 7;
    if (rem > 4 || (rem == 4 && (sticky || (kl & 1)))) { kl++; kh += kl == 0; }
    if ((uint32_t)e - 1u >= 2046u) return 0;
    *rh = sg | (((uint32_t)(e - 1) << 20) + kh); *rl = kl;
    return 1;
}
double __adddf3(double x, double y)
{
    uint64_t a = bits(x), b = bits(y); uint32_t rh, rl;
    if (add32(a >> 32, (uint32_t)a, b >> 32, (uint32_t)b, &rh, &rl)) return mk2(rh, rl);
    return add_mag(a, b, 0);
}
double __subdf3(double x, double y)
{
    uint64_t a = bits(x), b = bits(y); uint32_t rh, rl;
    if (add32(a >> 32, (uint32_t)a, (b >> 32) ^ 0x80000000u, (uint32_t)b, &rh, &rl)) return mk2(rh, rl);
    return add_mag(a, b, 1);
}
double __muldf3(double x, double y)
{
    uint64_t a = bits(x), b = bits(y);
    uint32_t ah = a >> 32, al = (uint32_t)a, bh = b >> 32, bl = (uint32_t)b;
    uint32_t ea = ah >> 20 & 0x7FF, eb = bh >> 20 & 0x7FF;
    if (ea - 1u >= 2046u || eb - 1u >= 2046u) {
        if ((!(ah << 1 | al) && eb != 0x7FF) || (!(bh << 1 | bl) && ea != 0x7FF)) return mk2((ah ^ bh) & 0x80000000u, 0);   /* 0 * finite */
        return mul_general(a, b);
    }
    uint32_t mh = (ah & 0xFFFFFu) | 0x100000u, nh = (bh & 0xFFFFFu) | 0x100000u;
    uint32_t llh = (uint32_t)(((uint64_t)al * bl) >> 32), w0 = al * bl;
    uint32_t lhh = (uint32_t)(((uint64_t)al * nh) >> 32), lhl = al * nh;
    uint32_t hlh = (uint32_t)(((uint64_t)mh * bl) >> 32), hll = mh * bl;
    uint32_t hhh = (uint32_t)(((uint64_t)mh * nh) >> 32), hhl = mh * nh;
    uint32_t w1 = llh + lhl; uint32_t c1 = w1 < llh; w1 += hll; c1 += w1 < hll;
    uint32_t w2 = lhh + hlh; uint32_t c2 = w2 < lhh; w2 += hhl; c2 += w2 < hhl; w2 += c1; c2 += w2 < c1;
    uint32_t w3 = hhh + c2;
    int e = (int)(ea + eb) - 1023;
    if (w3 >> 9) e++;                                   /* product >= 2^105 */
    else { w3 = w3 << 1 | w2 >> 31; w2 = w2 << 1 | w1 >> 31; w1 = w1 << 1 | w0 >> 31; w0 <<= 1; }
    uint32_t kh = w3 << 11 | w2 >> 21, kl = w2 << 11 | w1 >> 21;
    uint32_t guard = w1 >> 20 & 1, sticky = (w1 & 0xFFFFFu) | w0;
    if (guard && (sticky || (kl & 1))) { kl++; kh += kl == 0; }
    if ((uint32_t)e - 1u >= 2046u) return mul_general(a, b);
    return mk2(((ah ^ bh) & 0x80000000u) | (((uint32_t)(e - 1) << 20) + kh), kl);
}

static inline int cmp32(double x, double y, int unordered)
{
    uint64_t a = bits(x), b = bits(y);
    uint32_t ah = a >> 32, al = (uint32_t)a, bh = b >> 32, bl = (uint32_t)b;
    uint32_t am = ah & 0x7FFFFFFFu, bm = bh & 0x7FFFFFFFu;
    if (am > 0x7FF00000u || (am == 0x7FF00000u && al) || bm > 0x7FF00000u || (bm == 0x7FF00000u && bl)) return unordered;
    if (!(am | al | bm | bl)) return 0;                  /* +0 == -0 */
    if ((ah ^ bh) >> 31) return ah >> 31 ? -1 : 1;      /* signs differ */
    int gt = am > bm || (am == bm && al > bl), lt = am < bm || (am == bm && al < bl);
    int r = gt - lt;
    return ah >> 31 ? -r : r;
}
int __eqdf2(double a, double b) { return cmp32(a, b, 1); }
int __nedf2(double a, double b) { return cmp32(a, b, 1); }
int __ltdf2(double a, double b) { return cmp32(a, b, 1); }
int __ledf2(double a, double b) { return cmp32(a, b, 1); }
int __gtdf2(double a, double b) { return cmp32(a, b, -1); }
int __gedf2(double a, double b) { return cmp32(a, b, -1); }

double __floatsidf(int32_t v)
{
    if (!v) return mk(0);
    uint32_t sg = (uint32_t)v & 0x80000000u, u = v < 0 ? -(uint32_t)v : (uint32_t)v;
    int lz = clz32_(u);
    uint32_t m = u << lz;                                /* leading bit 31 */
    return mk2(sg | ((uint32_t)(1023 + 31 - lz) << 20) | (m << 1) >> 12, m << 21);
}
double __floatunsidf(uint32_t u)
{
    if (!u) return mk(0);
    int lz = clz32_(u);
    uint32_t m = u << lz;
    return mk2(((uint32_t)(1023 + 31 - lz) << 20) | (m << 1) >> 12, m << 21);
}
int32_t __fixdfsi(double d)
{
    uint64_t u = bits(d); uint32_t h = u >> 32, l = (uint32_t)u;
    int e = (int)(h >> 20 & 0x7FF) - 1023;
    int neg = h >> 31;
    if ((h & 0x7FFFFFFFu) > 0x7FF00000u || ((h & 0x7FFFFFFFu) == 0x7FF00000u && l)) return 0;   /* NaN */
    if (e < 0) return 0;
    if (e > 30) return neg ? INT32_MIN : INT32_MAX;
    uint32_t m = (h & 0xFFFFFu) << 11 | 1u << 31 | l >> 21;     /* the 53-bit mantissa, top 32 bits */
    uint32_t v = m >> (31 - e);
    return neg ? -(int32_t)v : (int32_t)v;
}
#endif

/* ---- compares (libgcc contract) ------------------------------------------ */
static int cmp(uint64_t a, uint64_t b, int unordered)
{
    if (isnan_(a) || isnan_(b)) return unordered;
    if (!((a | b) & ~SIGN)) return 0;                    /* +0 == -0 */
    int64_t ia = (a & SIGN) ? -(int64_t)(a & ~SIGN) : (int64_t)a;
    int64_t ib = (b & SIGN) ? -(int64_t)(b & ~SIGN) : (int64_t)b;
    return ia < ib ? -1 : ia > ib;
}
#if !SOC_CPU_RISCV
int __eqdf2(double a, double b) { return cmp(bits(a), bits(b), 1); }
int __nedf2(double a, double b) { return cmp(bits(a), bits(b), 1); }
int __ltdf2(double a, double b) { return cmp(bits(a), bits(b), 1); }
int __ledf2(double a, double b) { return cmp(bits(a), bits(b), 1); }
int __gtdf2(double a, double b) { return cmp(bits(a), bits(b), -1); }
int __gedf2(double a, double b) { return cmp(bits(a), bits(b), -1); }
#endif
int __unorddf2(double a, double b) { return isnan_(bits(a)) || isnan_(bits(b)); }

/* ---- integer conversions --------------------------------------------------- */
static double from_u64(uint64_t sign, uint64_t v)
{
    if (!v) return mk(0);
    return round_pack(sign, 63, v, 0);
}
#if !SOC_CPU_RISCV
double __floatsidf(int32_t v)   { return v < 0 ? from_u64(SIGN, -(uint64_t)(int64_t)v) : from_u64(0, (uint64_t)v); }
double __floatunsidf(uint32_t v) { return from_u64(0, v); }
#endif
double __floatdidf(int64_t v)   { return v < 0 ? from_u64(SIGN, -(uint64_t)v) : from_u64(0, (uint64_t)v); }
double __floatundidf(uint64_t v) { return from_u64(0, v); }

/* truncate toward zero; out of range saturates, NaN -> 0 */
static uint64_t to_u64(uint64_t u, int *neg, int *big)
{
    *neg = (u & SIGN) != 0; *big = 0;
    if (isnan_(u)) return 0;
    int be = (int)((u >> 52) & 0x7FF), e = be - 1023;
    if (e < 0) return 0;
    if (e > 63) { *big = 1; return ~0ull; }
    uint64_t m = (u & MANM) | (1ull << 52);
    return e >= 52 ? shl(m, e - 52) : shr(m, 52 - e);
}
#if !SOC_CPU_RISCV
int32_t __fixdfsi(double d)
{
    int n, b; uint64_t v = to_u64(bits(d), &n, &b);
    if (n) return v > 0x80000000ull ? INT32_MIN : (int32_t)(0u - (uint32_t)v);
    return v > 0x7FFFFFFF ? INT32_MAX : (int32_t)v;
}
#endif
uint32_t __fixunsdfsi(double d) { int n, b; uint64_t v = to_u64(bits(d), &n, &b); return n ? 0 : v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }
int64_t __fixdfdi(double d)
{
    int n, b; uint64_t v = to_u64(bits(d), &n, &b);
    if (n) return v > SIGN ? INT64_MIN : -(int64_t)v;
    return v > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)v;
}
uint64_t __fixunsdfdi(double d) { int n, b; uint64_t v = to_u64(bits(d), &n, &b); return n ? 0 : v; }
