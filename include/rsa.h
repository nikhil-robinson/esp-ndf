/* RSA / big-number accelerator (TRM 20): modular exponentiation and multiplication, 512..4096-bit operands.
 * Numbers are little-endian 32-bit word arrays. A modulus is prepared once (rsa_mod_init: M', R^2 mod M), then reused,
 * as mbedtls caches RR in an RSA key. The modulus must be odd. */
#pragma once
#include "ndf.h"

#define RSA_MAX_WORDS 128
typedef struct { int words; uint32_t mprime; uint32_t m[RSA_MAX_WORDS], rr[RSA_MAX_WORDS]; } rsa_mod_t;

int rsa_mod_init(rsa_mod_t *mod, const uint32_t *m, int words);              /* words 16..128, m odd */
int rsa_exp_mod(const rsa_mod_t *mod, uint32_t *z, const uint32_t *x, const uint32_t *y, int y_words);   /* z = x^y mod m */
int rsa_mul_mod(const rsa_mod_t *mod, uint32_t *z, const uint32_t *x, const uint32_t *y);               /* z = x*y mod m */
