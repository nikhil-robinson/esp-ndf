/* 64-bit divide helpers the compiler calls (we link no libgcc: it is
 * built for the windowed ABI, we run CALL0). Shift-subtract, at most 64
 * iterations: bounded time. */
#include <stdint.h>

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

/* count leading zeros without a clz instruction (RV32 without Zbb; Xtensa has nsau and never calls these) */
int __clzsi2(uint32_t x)
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
