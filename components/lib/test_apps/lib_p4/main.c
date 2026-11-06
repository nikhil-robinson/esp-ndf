/* crc32 / md5 / printf / strtod / qsort cycles on the P4; the same operations run in bench/idf_p4_lib. */
#include "console.h"
#include "cpu.h"
#include "crc32.h"
#include "md5.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define NOW() ccount()
#define CRC32(b, n) crc32_le(0, b, n)
#define MD5(b, n) do { uint8_t d_[16]; md5(b, n, d_); } while (0)
#define PRINT(name, v) printf("  %-22s %6lu cycles\n", name, (unsigned long)(v))
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static uint64_t ref_div(uint64_t n, uint64_t d, uint64_t *rem)      /* shift-subtract, what the library did before */
{
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) { r = r << 1 | ((n >> i) & 1); if (r >= d) { r -= d; q |= 1ull << i; } }
    *rem = r; return q;
}
static int check(void)
{
    int bad = 0;
    { uint64_t x = 88172645463325252ull;               /* 64-bit division and remainder against shift-subtract, operands of every magnitude */
      for (int i = 0; i < 20000; i++) {
          x ^= x << 13; x ^= x >> 7; x ^= x << 17; uint64_t n = x;
          x ^= x << 13; x ^= x >> 7; x ^= x << 17; uint64_t d = x;
          n >>= (i >> 3) & 63; d >>= (i * 7 >> 2) & 63; if (!d) d = 1; if (i % 11 == 0) n = ~0ull; if (i % 13 == 0) d = ~0ull >> (i & 63);
          if (!d) d = 1;
          uint64_t r, q = ref_div(n, d, &r);
          volatile uint64_t vn = n, vd = d;
          if (vn / vd != q || vn % vd != r) bad++;
          volatile int64_t sn = (int64_t)n, sd = (int64_t)d | 1;
          int64_t sq = sn / sd, sr = sn % sd; if (sq * sd + sr != sn) bad++;
      } }
    static uint8_t b[300]; for (int i = 0; i < 300; i++) b[i] = i * 13 + 5;
    for (int off = 0; off < 4; off++)                 /* crc32 against the bitwise definition, lengths 0..70 and 100..300 at four alignments */
        for (int n = 0; n < 300; n += (n < 70 ? 1 : 29)) {
            uint32_t r = ~0u; for (int i = 0; i < n; i++) { r ^= b[off + i]; for (int k = 0; k < 8; k++) r = r >> 1 ^ (r & 1 ? 0xEDB88320u : 0); }
            if (crc32_le(0, b + off, n) != ~r) bad++;
            if (n > 3 && crc32_le(crc32_le(0, b + off, 3), b + off + 3, n - 3) != ~r) bad++;     /* chained */
        }
    static const struct { const char *s, *h; } v[] = { { "", "d41d8cd98f00b204e9800998ecf8427e" }, { "abc", "900150983cd24fb0d6963f7d28e17f72" },
        { "12345678901234567890123456789012345678901234567890123456789012345678901234567890", "57edf4a22be3c955ac49da2e2107b67a" } };
    for (unsigned i = 0; i < 3; i++) { uint8_t d[16]; char hx[33]; md5(v[i].s, strlen(v[i].s), d); for (int k = 0; k < 16; k++) snprintf(hx + 2 * k, 3, "%02x", d[k]); if (strcmp(hx, v[i].h)) bad++; }
    { static char u[80] __attribute__((aligned(4))); strcpy(u + 1, v[2].s); uint8_t d[16], e[16]; md5(u + 1, 80, d); md5(v[2].s, 80, e); if (memcmp(d, e, 16)) bad++; }   /* unaligned input */
    if (strtol("123456", 0, 10) != 123456 || strtol("-42xyz", 0, 10) != -42 || strtol("  +7", 0, 0) != 7 || strtol("0x1f", 0, 0) != 31 || strtol("2147483647", 0, 10) != 2147483647L ||
        strtol("2147483648", 0, 10) != 2147483647L || strtol("-2147483649", 0, 10) != -2147483647L - 1 || strtol("123456789012", 0, 10) != 2147483647L || strtoull("18446744073709551615", 0, 10) != 18446744073709551615ull ||
        strtoull("18446744073709551616", 0, 10) != 18446744073709551615ull || strtoll("9223372036854775807", 0, 10) != 9223372036854775807ll || atoi("999999999") != 999999999) bad++;
    char *e; if (strtol("12ab", &e, 10) != 12 || *e != 'a') bad++;
    return bad;
}
int main(void)
{
    int bad = check();
    printf("\nlib_p4: crc32 / md5 / strtol checks: %d wrong\n", bad);
    printf("\nlib_p4 (cycles, best of 8)\n");
    static uint8_t buf[4096]; for (int i = 0; i < 4096; i++) buf[i] = i * 7 + (i >> 8);
    static int arr[1000], arr0[1000]; unsigned s = 12345; for (int i = 0; i < 1000; i++) { s = s * 1664525u + 1013904223u; arr0[i] = (int)(s >> 8); }
    static char out[64]; volatile uint32_t sink = 0; uint32_t t, best;
#define BEST(name, stmt) do { best = ~0u; for (int r_ = 0; r_ < 8; r_++) { t = NOW(); stmt; t = NOW() - t; if (t < best) best = t; } PRINT(name, best); } while (0)
    BEST("crc32 4 KB", sink += CRC32(buf, 4096));
    BEST("md5 4 KB", MD5(buf, 4096));
    BEST("snprintf %d", snprintf(out, sizeof out, "%d", 123456));
    BEST("snprintf %.3f", snprintf(out, sizeof out, "%.3f", 3.14159265));
    BEST("snprintf %s %5d %x", snprintf(out, sizeof out, "%s %5d %x", "abc", 42, 255));
    BEST("strtod 3.14159265", sink += (uint32_t)strtod("3.14159265", 0));
    BEST("strtol 123456", sink += strtol("123456", 0, 10));
    BEST("qsort 1000 ints", (memcpy(arr, arr0, sizeof arr), qsort(arr, 1000, sizeof arr[0], cmp)));
    { volatile double da = 1.2345678901, db = 9.8765432109; volatile float fa = 1.2345f, fb = 9.8765f; volatile int64_t la = 1234567890123456789ll, lb = 987654321ll; volatile uint32_t ua = 123456789u, ub = 12345u; volatile double dr;
    BEST("double add x100", for (int i = 0; i < 100; i++) dr = da + db);
    BEST("double mul x100", for (int i = 0; i < 100; i++) dr = da * db);
    BEST("double div x100", for (int i = 0; i < 100; i++) dr = da / db);
    BEST("double sqrt x100", for (int i = 0; i < 100; i++) dr = sqrt(da));
    BEST("double sin x100", for (int i = 0; i < 100; i++) dr = sin(da + i));
    BEST("double exp x100", for (int i = 0; i < 100; i++) dr = exp(da + i * 0.01));
    BEST("double->int x100", for (int i = 0; i < 100; i++) sink += (int)(da * i));
    BEST("float div x100", for (int i = 0; i < 100; i++) sink += (int)(fa / fb));
    BEST("int64 div x100", for (int i = 0; i < 100; i++) sink += (uint32_t)(la / (lb + i)));
    BEST("uint32 div x100", for (int i = 0; i < 100; i++) sink += ua / (ub + i)); }
    printf(bad ? "lib_p4 FAILED\n" : "lib_p4 done\n");
    for (;;) ;
}
