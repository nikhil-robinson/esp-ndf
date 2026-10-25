/* Hardware RNG statistics. */
#include "unit.h"
#include "console.h"
#include <string.h>
#include "rng.h"
TEST(rng_quality)                                       /* 64 KB from rng_hw: monobit and byte-frequency sanity (FIPS-style) */
{
    static uint32_t cnt[256]; memset(cnt, 0, sizeof cnt);
    uint32_t ones = 0, n = 16384;
    for (uint32_t i = 0; i < n; i++) { uint32_t w = rng_hw(); for (uint32_t v = w; v; v &= v - 1) ones++; for (int b = 0; b < 4; b++) cnt[(w >> (8 * b)) & 255]++; }
    double chi = 0, e = n * 4 / 256.0;
    for (int i = 0; i < 256; i++) chi += (cnt[i] - e) * (cnt[i] - e) / e;
    printf("      rng_hw: ones %lu of %lu (%.4f), byte chi2 %.1f (255 dof, 99.9%% bound 330)\n", ones, n * 32, ones / (n * 32.0), chi);
    UASSERT(ones > n * 16 - 1500 && ones < n * 16 + 1500 && chi < 330);
}

