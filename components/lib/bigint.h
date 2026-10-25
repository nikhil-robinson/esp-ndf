/* Small unsigned big integers for exact decimal <-> binary conversion
 * (strtod, printf). Fixed capacity on the stack, 32-bit limbs, little end
 * first. Library-internal. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define BIG_LIMBS 150                       /* 4800 bits: 10^1101 * 2^1024 and 10^1126 * 2^60 fit */
typedef struct { int n; uint32_t w[BIG_LIMBS]; } big_t;

void big_set_u64(big_t *b, uint64_t v);
void big_mul_small(big_t *b, uint32_t m);    /* b *= m */
void big_add_small(big_t *b, uint32_t a);    /* b += a */
void big_mul_pow10(big_t *b, int e);         /* b *= 10^e, e >= 0 */
void big_shl(big_t *b, int k);               /* b <<= k, k >= 0 */
int  big_cmp(const big_t *a, const big_t *b);
int  big_bitlen(const big_t *b);
uint32_t big_divmod_1e9(big_t *b);           /* b /= 1e9, returns the remainder */
bool big_shr(big_t *b, int k);               /* b >>= k, true if 1 bits were lost */
