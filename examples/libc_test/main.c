/* libc + float runtime: divide/sqrt/conversions bit-exact against host IEEE
 * (numpy) vectors, printf/snprintf formats against C reference strings,
 * strtol/strtof, qsort/bsearch, plus timings. */
#include "console.h"
#include "xtensa.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "vectors.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

typedef union { float f; uint32_t u; } fu_t;
typedef union { double d; uint64_t u; } du_t;
static bool same(uint32_t a, uint32_t b) { return a == b || ((a & 0x7FFFFFFF) > 0x7F800000 && (b & 0x7FFFFFFF) > 0x7F800000); }
static float vol_div(float a, float b) { volatile float x = a, y = b; return x / y; }
float sqrtf(float);
float ndf_divsf3_soft(float, float);
float ndf_sqrtf_soft(float);

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static int arr[2000];

int main(void)
{
    printf("libc_test\n");
    int bad = 0, first = -1;
    for (int i = 0; i < NV; i++) {
        fu_t a = { .u = va[i] }, b = { .u = vb[i] };
        fu_t q = { vol_div(a.f, b.f) };
        if (!same(q.u, vq[i])) { if (first < 0) first = i; bad++; }
    }
    CHECK(!bad, "divide: %d/%d bit-exact%s", NV - bad, NV, bad ? "" : "");
    if (bad) { fu_t a = { .u = va[first] }, b = { .u = vb[first] }; printf("     first: %08lx / %08lx = %08lx want %08lx\n", va[first], vb[first], (fu_t){ vol_div(a.f, b.f) }.u, vq[first]); }
    bad = 0; first = -1;
    for (int i = 0; i < NV; i++) {
        fu_t a = { .u = va[i] & 0x7FFFFFFF }, s = { sqrtf(a.f) };
        if (!same(s.u, vs[i])) { if (first < 0) first = i; bad++; }
    }
    CHECK(!bad, "sqrtf: %d/%d bit-exact", NV - bad, NV);
    bad = 0;
    for (int i = 0; i < NV; i++) {
        fu_t a = { .u = va[i] }, b = { .u = vb[i] }, q = { ndf_divsf3_soft(a.f, b.f) };
        fu_t s = { .u = va[i] & 0x7FFFFFFF }; s.f = ndf_sqrtf_soft(s.f);
        if (!same(q.u, vq[i]) || !same(s.u, vs[i])) bad++;
    }
    CHECK(!bad, "portable divide + sqrt (no-FPU chips): %d mismatches", bad);
    if (bad) { fu_t a = { .u = va[first] & 0x7FFFFFFF }; printf("     first: sqrt %08lx = %08lx want %08lx\n", a.u, (fu_t){ sqrtf(a.f) }.u, vs[first]); }
    bad = 0;
    for (int i = 0; i < NV; i++) { volatile float f; fu_t a = { .u = va[i] }; f = a.f; du_t d = { (double)f }; if (d.u != vd[i] && !((va[i] & 0x7FFFFFFF) > 0x7F800000)) bad++; }
    CHECK(!bad, "float -> double: %d mismatches", bad);
    bad = 0; first = -1;
    for (int i = 0; i < ND; i++) { volatile double d; du_t x = { .u = dd[i] }; d = x.d; fu_t f = { (float)d }; if (!same(f.u, df[i])) { if (first < 0) first = i; bad++; } }
    CHECK(!bad, "double -> float (rounding, denormals, overflow): %d mismatches", bad);
    if (bad) { du_t x = { .u = dd[first] }; printf("     first: %016llx -> %08lx want %08lx\n", dd[first], (fu_t){ (float)x.d }.u, df[first]); }
    bad = 0;
    for (int i = 0; i < ND; i++) { volatile int64_t v = ll[i]; fu_t f = { (float)v }; if (f.u != lf[i]) bad++; }
    CHECK(!bad, "int64 -> float: %d mismatches", bad);
    bad = 0;
    for (int i = 0; i < ND; i++) { fu_t f = { .u = lf[i] }; volatile float vf = f.f; int64_t back = (int64_t)vf; if ((float)back != vf) bad++; }
    CHECK(!bad, "float -> int64 round trip: %d mismatches", bad);

    char buf[256]; bad = 0;
    for (unsigned i = 0; i < sizeof fcase / sizeof fcase[0]; i++) {
        snprintf(buf, sizeof buf, fcase[i].fmt, fcase[i].v);
        if (strcmp(buf, fcase[i].want)) { bad++; printf("     \"%s\": got \"%s\" want \"%s\"\n", fcase[i].fmt, buf, fcase[i].want); }
    }
    CHECK(!bad, "printf floats: %u/%u formats match C", (unsigned)(sizeof fcase / sizeof fcase[0]) - bad, (unsigned)(sizeof fcase / sizeof fcase[0]));
    snprintf(buf, sizeof buf, "[%5d|%-5d|%05d|%+d|% d|%.3d|%x|%08lX|%s|%.2s|%c|%lld|%llu|%%]", 42, 42, -42, 7, 7, 5, 255u, 0xBEEFul, "hi", "abcdef", 'z', -9000000000ll, 18000000000000000000ull);
    CHECK(!strcmp(buf, "[   42|42   |-0042|+7| 7|005|ff|0000BEEF|hi|ab|z|-9000000000|18000000000000000000|%]"), "printf ints/strings: %s", buf);
    int n = snprintf(buf, 8, "%s", "truncate-me");
    CHECK(n == 11 && !strcmp(buf, "truncat"), "snprintf truncates, returns full length (%d)", n);
    float pf = 1.5f; snprintf(buf, sizeof buf, "%.3f", pf);
    CHECK(!strcmp(buf, "1.500"), "float argument promoted: %s", buf);

    char *e;
    CHECK(strtol("  -1234xyz", &e, 10) == -1234 && *e == 'x', "strtol decimal + end pointer");
    CHECK(strtol("0x7fffffff", 0, 0) == 0x7fffffff && strtol("0x80000000", 0, 0) == LONG_MAX && strtol("-0x80000001", 0, 16) == LONG_MIN, "strtol hex + saturation");
    CHECK(strtoul("0777", 0, 0) == 0777 && strtoul("z", &e, 36) == 35 && strtoull("18446744073709551615", 0, 10) == ULLONG_MAX, "strtoul octal/base36, strtoull max");
    CHECK(strtoll("-9223372036854775808", 0, 10) == LLONG_MIN && atoi("  +56") == 56, "strtoll min, atoi");
    const char *fs[] = { "3.14159", "-2.5e-3", "1e10", "0.1", "16777216", "1.17549435e-38", "3.4028235e38", "inf", "-0", "123456789012345678901234", "1e-45", ".5" };
    const float fv[] = { 3.14159f, -2.5e-3f, 1e10f, 0.1f, 16777216.0f, 1.17549435e-38f, 3.4028235e38f, __builtin_inff(), -0.0f, 123456789012345678901234.0f, 1e-45f, 0.5f };
    bad = 0; int ulp1 = 0;
    for (int i = 0; i < 12; i++) {
        fu_t got = { strtof(fs[i], 0) }, want = { fv[i] };
        int d = (int)(got.u - want.u);
        if (d) { if (d == 1 || d == -1) ulp1++; else { bad++; printf("     strtof(\"%s\") = %08lx want %08lx\n", fs[i], got.u, want.u); } }
    }
    CHECK(!bad, "strtof: 12 cases, %d exact, %d within 1 ulp", 12 - ulp1 - bad, ulp1);

    uint32_t x = 1; for (int i = 0; i < 2000; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; arr[i] = (int)(x % 100000) - 50000; }
    uint32_t c0 = ccount(); qsort(arr, 2000, sizeof(int), cmp_int); uint32_t qc = ccount() - c0;
    int sorted = 1; for (int i = 1; i < 2000; i++) if (arr[i - 1] > arr[i]) sorted = 0;
    CHECK(sorted, "qsort 2000 ints: %lu cycles (%lu per element)", (unsigned long)qc, (unsigned long)qc / 2000);
    for (int i = 0; i < 2000; i++) arr[i] = i;
    qsort(arr, 2000, sizeof(int), cmp_int);
    int key = 1234, *hit = bsearch(&key, arr, 2000, sizeof(int), cmp_int); key = 5000;
    CHECK(hit && *hit == 1234 && !bsearch(&key, arr, 2000, sizeof(int), cmp_int), "qsort sorted input, bsearch hit/miss");

    volatile float fa = 1.2345f, fb = 6.789f; float r = 0;
    c0 = ccount(); for (int i = 0; i < 100; i++) r += fa / fb; uint32_t dc = (ccount() - c0) / 100;
    c0 = ccount(); for (int i = 0; i < 100; i++) r += sqrtf(fb); uint32_t sc = (ccount() - c0) / 100;
    c0 = ccount(); for (int i = 0; i < 10; i++) snprintf(buf, sizeof buf, "%.3f", 3.14159); uint32_t pc = (ccount() - c0) / 10;
    c0 = ccount(); for (int i = 0; i < 10; i++) snprintf(buf, sizeof buf, "%d", 123456); uint32_t ic = (ccount() - c0) / 10;
    c0 = ccount(); for (int i = 0; i < 10; i++) (void)strtof("3.14159", 0); uint32_t tc = (ccount() - c0) / 10;
    printf("     float divide %lu, sqrtf %lu, snprintf %%.3f %lu, %%d %lu, strtof %lu cycles (r=%d)\n",
           (unsigned long)dc, (unsigned long)sc, (unsigned long)pc, (unsigned long)ic, (unsigned long)tc, (int)r);
    printf("=== %s (%d failures) ===\n", fails ? "FAILED" : "ALL PASS", fails);
    for (;;) {}
}
