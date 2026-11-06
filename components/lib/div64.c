/* 64-bit divide helpers the compiler calls (we link no libgcc: it is
 * built for the windowed ABI, we run CALL0). Shift-subtract, at most 64
 * iterations: bounded time. */
#include <stdint.h>
#include "soc_caps.h"

#if SOC_CPU_RISCV
/* RV32 with the M extension: the 64-bit quotient from the 32-bit divider (Hacker's Delight divlu / udivdi3), a handful of divisions
 * instead of 64 shift-subtract steps (1350 cycles per division before). */
static inline __attribute__((always_inline)) int clz32(uint32_t x)      /* x != 0: through the FPU, no call */
{
    x &= ~(x >> 1);                                   /* no two adjacent ones: the conversion cannot round up to the next power of two */
    union { float f; uint32_t u; } c = { .f = (float)x };
    return 158 - (int)(c.u >> 23);
}
#define CLZ clz32
static inline __attribute__((always_inline)) uint32_t divlu(uint32_t u1, uint32_t u0, uint32_t v)       /* (u1:u0) / v, u1 < v, v != 0 */
{
    const uint32_t b = 65536;
    int s = clz32(v);
    v <<= s;
    uint32_t vn1 = v >> 16, vn0 = v & 0xFFFF;
    uint32_t un32 = s ? (u1 << s) | (u0 >> (32 - s)) : u1;
    uint32_t un10 = u0 << s;
    uint32_t un1 = un10 >> 16, un0 = un10 & 0xFFFF;
    uint32_t q1 = un32 / vn1; __asm__("" : "+r"(q1)); uint32_t rhat = un32 - q1 * vn1;   /* not a second divide (remu) */
    while (q1 >= b || q1 * vn0 > b * rhat + un1) { q1--; rhat += vn1; if (rhat >= b) break; }
    uint32_t un21 = un32 * b + un1 - q1 * v;
    uint32_t q0 = un21 / vn1; __asm__("" : "+r"(q0)); rhat = un21 - q0 * vn1;
    while (q0 >= b || q0 * vn0 > b * rhat + un0) { q0--; rhat += vn1; if (rhat >= b) break; }
    return q1 * b + q0;
}

static inline __attribute__((always_inline)) uint64_t udiv_q(uint64_t n, uint64_t d)
{
    uint32_t nh = (uint32_t)(n >> 32), nl = (uint32_t)n, dh = (uint32_t)(d >> 32), dl = (uint32_t)d;
    if (!dh) {
        if (!nh) return dl ? nl / dl : ~0ull;           /* no trap: saturate */
        if (nh < dl) return divlu(nh, nl, dl);
        uint32_t qh = nh / dl, r = nh - qh * dl;
        return (uint64_t)qh << 32 | divlu(r, nl, dl);
    }
    int s = clz32(dh);                              /* quotient < 2^32 */
    uint32_t v1 = (uint32_t)((d << s) >> 32);
    uint64_t u1 = n >> 1;
    uint32_t q1 = divlu((uint32_t)(u1 >> 32), (uint32_t)u1, v1);   /* u1 < 2^63, v1 >= 2^31: u1 / 2^32 < v1 */
    uint32_t q0 = (uint32_t)(((uint64_t)q1 << s) >> 31);
    if (q0) q0--;
    if (n - (uint64_t)q0 * d >= d) q0++;
    return q0;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return udiv_q(n, d); }
uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem)
{
    if (d == 0) { if (rem) *rem = 0; return ~0ull; }
    uint64_t q = udiv_q(n, d);
    if (rem) *rem = n - q * d;
    return q;
}
uint64_t __umoddi3(uint64_t n, uint64_t d) { if (!d) return 0; return n - udiv_q(n, d) * d; }
int64_t __divdi3(int64_t n, int64_t d)
{
    int neg = (n < 0) ^ (d < 0);
    uint64_t q = udiv_q(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d);
    return neg ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t un = n < 0 ? -(uint64_t)n : (uint64_t)n, ud = d < 0 ? -(uint64_t)d : (uint64_t)d;
    uint64_t r = ud ? un - udiv_q(un, ud) * ud : 0;
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
#else
uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    if (d == 0) { if (rem) *rem = 0; return ~0ull; }   /* no trap: saturate */
    if (!(n >> 32) && !(d >> 32)) {                    /* fast path: 32-bit */
        uint32_t q32 = (uint32_t)n / (uint32_t)d;
        if (rem) *rem = (uint32_t)n - q32 * (uint32_t)d;
        return q32;
    }
    for (int i = 63; i >= 0; i--) {
        r = r << 1 | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ull << i; }
    }
    if (rem) *rem = r;
    return q;
}

#endif

#if !SOC_CPU_RISCV
uint64_t __udivdi3(uint64_t n, uint64_t d) { return __udivmoddi4(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; __udivmoddi4(n, d, &r); return r; }

int64_t __divdi3(int64_t n, int64_t d)
{
    int neg = (n < 0) ^ (d < 0);
    uint64_t q = __udivdi3(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t r = __umoddi3(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}

#endif

/* 64-bit shifts by a variable count */
uint64_t __ashldi3(uint64_t a, int b)
{
    uint32_t lo = (uint32_t)a, hi = (uint32_t)(a >> 32);
    if (b & 32) { hi = lo << (b & 31); lo = 0; }
    else if (b) { hi = hi << b | lo >> (32 - b); lo <<= b; }
    return (uint64_t)hi << 32 | lo;
}
uint64_t __lshrdi3(uint64_t a, int b)
{
    uint32_t lo = (uint32_t)a, hi = (uint32_t)(a >> 32);
    if (b & 32) { lo = hi >> (b & 31); hi = 0; }
    else if (b) { lo = lo >> b | hi << (32 - b); hi >>= b; }
    return (uint64_t)hi << 32 | lo;
}
int64_t __ashrdi3(int64_t a, int b)
{
    int32_t hi = (int32_t)(a >> 32); uint32_t lo = (uint32_t)a;
    if (b & 32) { lo = (uint32_t)(hi >> (b & 31)); hi >>= 31; }
    else if (b) { lo = lo >> b | (uint32_t)hi << (32 - b); hi >>= b; }
    return (int64_t)((uint64_t)(uint32_t)hi << 32 | lo);
}

#if SOC_CPU_RISCV
/* count leading zeros without a clz instruction (RV32 without Zbb; Xtensa has nsau and never calls these) */
int __clzsi2(uint32_t x)
{
    if (!x) return 32;
    x &= ~(x >> 1);                                   /* no two adjacent ones: the float conversion cannot round up to the next power of two */
    union { float f; uint32_t u; } c = { .f = (float)x };
    return 158 - (int)(c.u >> 23);
}
int __clzsi2_slow(uint32_t x)
{
    int n = 0;
    if (!x) return 32;
    if (!(x >> 16)) { n += 16; x <<= 16; }
    if (!(x >> 24)) { n += 8; x <<= 8; }
    if (!(x >> 28)) { n += 4; x <<= 4; }
    if (!(x >> 30)) { n += 2; x <<= 2; }
    return n + !(x >> 31);
}
int __clzdi2(uint64_t x)
{
    uint32_t hi = (uint32_t)(x >> 32);
    return hi ? __clzsi2(hi) : 32 + __clzsi2((uint32_t)x);
}
int __ctzsi2(uint32_t x) { return x ? 31 - __clzsi2(x & -x) : 32; }
int __ctzdi2(uint64_t x) { uint32_t lo = (uint32_t)x; return lo ? __ctzsi2(lo) : 32 + __ctzsi2((uint32_t)(x >> 32)); }
int __popcountsi2(uint32_t x) { x -= x >> 1 & 0x55555555u; x = (x & 0x33333333u) + (x >> 2 & 0x33333333u); return (int)(((x + (x >> 4)) & 0x0F0F0F0Fu) * 0x01010101u >> 24); }
uint32_t __bswapsi2(uint32_t x) { return x >> 24 | (x >> 8 & 0xFF00u) | (x << 8 & 0xFF0000u) | x << 24; }
uint64_t __bswapdi2(uint64_t x) { return (uint64_t)__bswapsi2((uint32_t)x) << 32 | __bswapsi2((uint32_t)(x >> 32)); }
#endif
