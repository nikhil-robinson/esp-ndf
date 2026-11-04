/* libgcc helpers RV32 needs beyond components/lib/div64.c (no Zbb: no clz / ctz / cpop instructions) */
#include <stdint.h>

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
