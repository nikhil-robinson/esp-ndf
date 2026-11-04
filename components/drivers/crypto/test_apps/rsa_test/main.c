/* RSA accelerator: x^65537, x^d (full-size exponent) and x*y mod m for 1024/2048/4096-bit moduli against Python's
 * pow() (bench/rsa/vectors.h); timing (sibling: bench/idf_rsa, same vectors). */
#include "console.h"
#include "rsa.h"
#include "sys.h"
#include "xtensa.h"
#include <string.h>
#include "rsa/vectors.h"

static rsa_mod_t mod;
static uint32_t z[RSA_MAX_WORDS];
static int fail;

static void one(int bitsz, const uint32_t *m, const uint32_t *x, const uint32_t *y, const uint32_t *d, const uint32_t *pub, const uint32_t *priv, const uint32_t *mul)
{
    int w = bitsz / 32;
    uint32_t t = ccount(); rsa_mod_init(&mod, m, w); uint32_t ti = ccount() - t;
    static const uint32_t e[1] = { 65537 };
    t = ccount(); rsa_exp_mod_public(&mod, z, x, e, 1); uint32_t tp = ccount() - t; int ok1 = !memcmp(z, pub, 4 * w);
    t = ccount(); rsa_exp_mod(&mod, z, x, d, w); uint32_t td = ccount() - t; int ok2 = !memcmp(z, priv, 4 * w);
    t = ccount(); rsa_mul_mod(&mod, z, x, y); uint32_t tm = ccount() - t; int ok3 = !memcmp(z, mul, 4 * w);
    printf("%s RSA-%d: x^65537 %d, x^d %d, x*y %d | prepare modulus %lu us, x^65537 %lu us, x^d %lu us, x*y %lu us\n",
           ok1 && ok2 && ok3 ? "PASS" : "FAIL", bitsz, ok1, ok2, ok3, (unsigned long)(ti / 240), (unsigned long)(tp / 240),
           (unsigned long)(td / 240), (unsigned long)(tm / 240));
    fail |= !(ok1 && ok2 && ok3);
    /* constant time: a 1-bit and a full-size secret exponent take the same time (the fast path would differ ~w*32x) */
    static uint32_t one_bit[RSA_MAX_WORDS]; memset(one_bit, 0, sizeof one_bit); one_bit[0] = 3;
    t = ccount(); rsa_exp_mod(&mod, z, x, one_bit, w); uint32_t t1 = ccount() - t;
    int ct = NDF_RSA_CONST_TIME ? (t1 > td - td / 100 && t1 < td + td / 100) : t1 < td / 4;   /* default off: a fixed key only leaks its length and weight */
    printf("%s RSA-%d timing (constant time %s): exponent 3 %lu us vs full %lu us\n", ct ? "PASS" : "FAIL", bitsz, NDF_RSA_CONST_TIME ? "on" : "off", (unsigned long)(t1 / 240), (unsigned long)(td / 240));
    fail |= !ct;
}

int main(void)
{
    delay_ms(200);
    one(1024, v1024_m, v1024_x, v1024_y, v1024_d, v1024_pub, v1024_priv, v1024_mul);
    one(2048, v2048_m, v2048_x, v2048_y, v2048_d, v2048_pub, v2048_priv, v2048_mul);
    one(4096, v4096_m, v4096_x, v4096_y, v4096_d, v4096_pub, v4096_priv, v4096_mul);
    printf("rsa_test %s done\n", fail ? "FAIL" : "PASS");
    for (;;) {}
}
