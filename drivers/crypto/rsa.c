/* RSA accelerator, ESP-IDF sequence (hal/mpi_hal.c, hal/esp32s3/include/hal/mpi_ll.h, mbedtls port bignum_alt.c
 * esp_mpi_exp_mpi_mod_hw_op): memory power on and wait for QUERY_CLEAN; length = words - 1; X, Y, M, Z = R^2 mod M;
 * M' = -M^-1 mod 2^32; search from the exponent's top bit, constant time off (as IDF); start, wait for the interrupt
 * flag, read Z. */
#include "rsa.h"
#include "clk.h"
#include "xtensa.h"
#include <string.h>

#define RSA_BASE   0x6003C000u
#define MEM_M      (RSA_BASE + 0x000)
#define MEM_Z      (RSA_BASE + 0x200)
#define MEM_Y      (RSA_BASE + 0x400)
#define MEM_X      (RSA_BASE + 0x600)
#define M_DASH     (RSA_BASE + 0x800)
#define LENGTH     (RSA_BASE + 0x804)
#define CLEAN      (RSA_BASE + 0x808)
#define MODEXP     (RSA_BASE + 0x80C)
#define MODMULT    (RSA_BASE + 0x810)
#define QUERY_INT  (RSA_BASE + 0x818)
#define CLEAR_INT  (RSA_BASE + 0x81C)
#define CONST_TIME (RSA_BASE + 0x820)
#define SEARCH_EN  (RSA_BASE + 0x824)
#define SEARCH_POS (RSA_BASE + 0x828)
#define INT_ENA    (RSA_BASE + 0x82C)
#define RSA_PD     0x600C0040u           /* SYSTEM_RSA_PD_CTRL: b0 memory power-down */

static bool up;
static void hw_on(void)
{
    if (up) return;
    periph_enable(PERIPH_RSA);
    REG_CLR(RSA_PD, BIT(0));
    for (uint32_t t = ccount(); !REG(CLEAN) && ccount() - t < 2400000; ) {}
    REG(INT_ENA) = 0;
    up = true;
}

static uint32_t mprime(uint32_t n0)       /* -n0^-1 mod 2^32 (Newton) */
{
    uint32_t x = n0;                     /* correct to 3 bits for odd n0 */
    for (int i = 0; i < 5; i++) x *= 2 - n0 * x;
    return 0u - x;
}

static int top(const uint32_t *a, int n) { while (n && !a[n - 1]) n--; return n; }
static int bits(const uint32_t *a, int n) { n = top(a, n); return n ? 32 * (n - 1) + 32 - __builtin_clz(a[n - 1]) : 0; }

static bool geq(const uint32_t *a, const uint32_t *b, int n)
{
    for (int i = n - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] > b[i];
    return true;
}
static void sub(uint32_t *a, const uint32_t *b, int n)
{
    uint64_t br = 0;
    for (int i = 0; i < n; i++) { uint64_t d = (uint64_t)a[i] - b[i] - br; a[i] = (uint32_t)d; br = d >> 63; }
}

int rsa_mod_init(rsa_mod_t *mod, const uint32_t *m, int words)
{
    if (words < 16 || words > RSA_MAX_WORDS || !(m[0] & 1)) return -1;
    mod->words = words;
    memcpy(mod->m, m, 4u * words);
    mod->mprime = mprime(m[0]);
    /* R^2 mod M, R = 2^(32 words): R mod M first (= R - M when the top bit is set, else 32*words doublings of 1),
     * then 32*words doublings, reducing as it goes (carry-aware) */
    uint32_t *r = mod->rr;
    int k0 = 0;
    if (m[words - 1] >> 31) { for (int i = 0; i < words; i++) r[i] = 0u - m[i] - (i ? 1 : 0); k0 = 32 * words; }
    else { memset(r, 0, 4u * words); r[0] = 1; }
    for (int k = k0; k < 64 * words; k++) {
        uint32_t carry = r[words - 1] >> 31;
        for (int i = words - 1; i > 0; i--) r[i] = r[i] << 1 | r[i - 1] >> 31;
        r[0] <<= 1;
        if (carry || geq(r, m, words)) sub(r, m, words);
    }
    return 0;
}

static void load(uint32_t base, const uint32_t *v, int n, int words)
{
    for (int i = 0; i < words; i++) REG(base + 4 * i) = i < n ? v[i] : 0;
}
static void run(uint32_t start, uint32_t *z, int words)
{
    REG(CLEAR_INT) = 1;
    REG(start) = 1;
    /* bounded wait (a 4096-bit x^d takes ~1.3 s): elapsed in kilocycles so the 32-bit counter can wrap; 10 s */
    uint32_t last = ccount(), kc = 0;
    while (!REG(QUERY_INT) && kc < 2400000u) { uint32_t n = ccount(); kc += (n - last) / 1000; last += (n - last) / 1000 * 1000; }
    REG(CLEAR_INT) = 1;
    for (int i = 0; i < words; i++) z[i] = REG(MEM_Z + 4 * i);
}

int rsa_exp_mod(const rsa_mod_t *mod, uint32_t *z, const uint32_t *x, const uint32_t *y, int y_words)
{
    int w = mod->words, yb = bits(y, y_words);
    if (y_words > w) return -1;
    hw_on();
    REG(LENGTH) = w - 1;
    load(MEM_X, x, w, w); load(MEM_Y, y, y_words, w); load(MEM_M, mod->m, w, w); load(MEM_Z, mod->rr, w, w);
    REG(M_DASH) = mod->mprime;
    REG(CONST_TIME) = 0;
    REG(SEARCH_EN) = 1;
    REG(SEARCH_POS) = yb ? yb - 1 : 0;
    run(MODEXP, z, w);
    REG(SEARCH_EN) = 0;
    return 0;
}

int rsa_mul_mod(const rsa_mod_t *mod, uint32_t *z, const uint32_t *x, const uint32_t *y)
{
    int w = mod->words;
    hw_on();
    REG(LENGTH) = w - 1;
    load(MEM_X, x, w, w); load(MEM_Y, y, w, w); load(MEM_M, mod->m, w, w); load(MEM_Z, mod->rr, w, w);
    REG(M_DASH) = mod->mprime;
    run(MODMULT, z, w);
    return 0;
}
