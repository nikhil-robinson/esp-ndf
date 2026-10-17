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
