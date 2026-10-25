#include "bigint.h"

void big_set_u64(big_t *b, uint64_t v)
{
    b->w[0] = (uint32_t)v; b->w[1] = (uint32_t)(v >> 32);
    b->n = b->w[1] ? 2 : b->w[0] ? 1 : 0;
}

void big_mul_small(big_t *b, uint32_t m)
{
    uint32_t carry = 0;
    for (int i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->w[i] * m + carry;
        b->w[i] = (uint32_t)t; carry = (uint32_t)(t >> 32);
    }
    if (carry && b->n < BIG_LIMBS) b->w[b->n++] = carry;
}

void big_add_small(big_t *b, uint32_t a)
{
    for (int i = 0; a && i < b->n; i++) {
        uint32_t t = b->w[i] + a;
        a = t < a; b->w[i] = t;
    }
    if (a && b->n < BIG_LIMBS) b->w[b->n++] = a;
}

void big_mul_pow10(big_t *b, int e)
{
    static const uint32_t p10[] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 };
    for (; e >= 9; e -= 9) big_mul_small(b, 1000000000);
    if (e) big_mul_small(b, p10[e]);
}

void big_shl(big_t *b, int k)
{
    if (!b->n) return;
    if (k <= 0) return;                                             /* k < 0: no shift (and no negative index) */
    int words = k >> 5, bits = k & 31;
    if (b->n + words + 1 > BIG_LIMBS) words = BIG_LIMBS - b->n - 1;   /* cannot happen within the callers' ranges */
    if (bits) {
        b->w[b->n] = 0;
        for (int i = b->n; i > 0; i--) b->w[i] = b->w[i] << bits | b->w[i - 1] >> (32 - bits);
        b->w[0] <<= bits;
        if (b->w[b->n]) b->n++;
    }
    if (words) {
        for (int i = b->n - 1; i >= 0; i--) b->w[i + words] = b->w[i];
        for (int i = 0; i < words; i++) b->w[i] = 0;
        b->n += words;
    }
}

int big_cmp(const big_t *a, const big_t *b)
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

int big_bitlen(const big_t *b)
{
    return b->n ? 32 * b->n - __builtin_clz(b->w[b->n - 1]) : 0;
}

/* floor(x / 1e9) for x < 2^62: x * ceil(2^93 / 1e9) >> 93, exact (error < 2^-31 < 1e-9) */
static inline uint64_t div1e9(uint64_t x)
{
    const uint64_t M = 0x89705F4136B4A598ULL;
    uint64_t xl = (uint32_t)x, xh = x >> 32, ml = (uint32_t)M, mh = M >> 32;
    uint64_t ll = xl * ml, lh = xl * mh, hl = xh * ml, hh = xh * mh;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    uint64_t hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    return hi >> 29;
}

uint32_t big_divmod_1e9(big_t *b)
{
    uint32_t r = 0;
    for (int i = b->n - 1; i >= 0; i--) {
        uint64_t x = (uint64_t)r << 32 | b->w[i];          /* r < 1e9: x < 2^62 */
        uint64_t q = div1e9(x);
        b->w[i] = (uint32_t)q;
        r = (uint32_t)(x - q * 1000000000u);
    }
    while (b->n && !b->w[b->n - 1]) b->n--;
    return r;
}

/* b >>= k; returns true if any 1 bit was shifted out */
bool big_shr(big_t *b, int k)
{
    if (k <= 0) return false;
    int words = k >> 5, bits = k & 31;
    bool lost = false;
    for (int i = 0; i < words && i < b->n; i++) lost |= b->w[i] != 0;
    if (words >= b->n) { b->n = 0; return lost; }
    for (int i = 0; i < b->n - words; i++) b->w[i] = b->w[i + words];
    b->n -= words;
    if (bits) {
        lost |= (b->w[0] & ((1u << bits) - 1)) != 0;
        for (int i = 0; i < b->n - 1; i++) b->w[i] = b->w[i] >> bits | b->w[i + 1] << (32 - bits);
        b->w[b->n - 1] >>= bits;
    }
    while (b->n && !b->w[b->n - 1]) b->n--;
    return lost;
}
