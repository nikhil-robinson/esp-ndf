/* RNG test and benchmark (sibling: bench/idf_rng). ChaCha20 known answer; statistics (monobit, runs, byte chi-square)
 * on hardware words and generator output; speed of rng_hw / rng_u32 / rng_fill. */
#include "console.h"
#include "rng.h"
#include "sys.h"
#include "xtensa.h"
#include <string.h>

static uint8_t big[65536];

static void stats(const char *name, const uint8_t *p, size_t n, int *fail)
{
    uint32_t ones = 0, runs = 1, cnt[256] = { 0 };
    for (size_t i = 0; i < n; i++) { { uint8_t v = p[i]; while (v) { ones += v & 1; v >>= 1; } } cnt[p[i]]++; }
    for (size_t i = 1; i < n * 8; i++) runs += ((p[i / 8] >> (i % 8)) & 1) != ((p[(i - 1) / 8] >> ((i - 1) % 8)) & 1);
    double e = n / 256.0, chi = 0; for (int i = 0; i < 256; i++) chi += (cnt[i] - e) * (cnt[i] - e) / e;
    double bits = n * 8.0, f1 = ones / bits, fr = runs / bits;
    bool ok = f1 > 0.49 && f1 < 0.51 && fr > 0.49 && fr < 0.51 && chi > 180 && chi < 340;   /* 255 dof, ~p 0.001..0.999 */
    printf("%s %s: %u bytes, ones %.4f, runs %.4f, chi2 %.1f (255 dof)\n", ok ? "PASS" : "FAIL", name, (unsigned)n, f1, fr, chi);
    *fail |= !ok;
}

int main(void)
{
    delay_ms(300);
    int fail = 0;
    int kat = rng_selftest();
    printf("%s ChaCha20 RFC 8439 known answer\n", kat ? "FAIL" : "PASS"); fail |= kat != 0;
    rng_init();
    uint32_t t = ccount(); for (int i = 0; i < 1024; i++) ((uint32_t *)big)[i] = rng_hw(); uint32_t hw = (ccount() - t) / 1024;
    stats("rng_hw", big, 4096, &fail);
    t = ccount(); rng_fill(big, sizeof big); uint32_t f64 = ccount() - t;
    stats("rng_fill", big, sizeof big, &fail);
    t = ccount(); rng_fill(big, 4096); uint32_t f4 = ccount() - t;
    uint32_t u = ~0u; for (int i = 0; i < 8; i++) { uint32_t c = ccount(); (void)rng_u32(); c = ccount() - c; if (c < u) u = c; }
    uint8_t a[32], b[32]; rng_fill(a, 32); rng_fill(b, 32);
    bool diff = memcmp(a, b, 32) != 0; fail |= !diff;
    printf("%s two 32-byte draws differ\n", diff ? "PASS" : "FAIL");
    printf("speed: rng_hw %lu cycles/word (%lu us), rng_u32 %lu cycles, rng_fill 4 KB %lu us, 64 KB %lu us (%lu MB/s)\n",
           (unsigned long)hw, (unsigned long)(hw / 240), (unsigned long)u, (unsigned long)(f4 / 240),
           (unsigned long)(f64 / 240), (unsigned long)(65536ull * 240 / f64));
    printf("rng_test %s done\n", fail ? "FAIL" : "PASS");
    for (;;) {}
}
