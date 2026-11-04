/* libc + float runtime: divide/sqrt/conversions bit-exact against host IEEE
 * (numpy) vectors, printf/snprintf formats against C reference strings,
 * strtol/strtof, qsort/bsearch, plus timings. */
#include "console.h"
#include "cpu.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "vectors.h"
#include "time_vectors.h"
#include <time.h>
#include <sys/time.h>
time_t timegm(struct tm *);                        /* GNU/BSD extension: not prototyped by default */
static inline uint32_t dcc(void) { return ccount(); }
#include "dbench.inc"

static int fails;
static uint32_t rnd_state = 0x12345678;
static uint32_t RND(void) { uint32_t x = rnd_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return rnd_state = x; }
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

typedef union { float f; uint32_t u; } fu_t;
typedef union { double d; uint64_t u; } du_t;
static bool dsame(uint64_t a, uint64_t b) { return a == b || ((a & ~(1ull << 63)) > 0x7FF0000000000000ull && (b & ~(1ull << 63)) > 0x7FF0000000000000ull); }
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

    /* double arithmetic */
    int ba = 0, bs = 0, bm = 0, bd = 0, bc = 0; first = -1;
    for (int i = 0; i < NDV; i++) {
        du_t x = { .u = xa[i] }, y = { .u = xb[i] };
        volatile double vx = x.d, vy = y.d;
        du_t r;
        r.d = vx + vy; if (!dsame(r.u, r_add[i])) { ba++; if (first < 0) first = i; }
        r.d = vx - vy; if (!dsame(r.u, r_sub[i])) bs++;
        r.d = vx * vy; if (!dsame(r.u, r_mul[i])) bm++;
        r.d = vx / vy; if (!dsame(r.u, r_div[i])) bd++;
        int c = r_cmp[i];
        if ((vx < vy) != (c == -1) || (vx > vy) != (c == 1) || (vx == vy) != (c == 0) || (vx <= vy) != (c == -1 || c == 0) ||
            (vx >= vy) != (c == 1 || c == 0) || (vx != vy) != (c != 0) || __builtin_isunordered(vx, vy) != (c == 2)) bc++;
    }
    CHECK(!(ba | bs | bm | bd), "double + - * /: %d vectors each, mismatches %d %d %d %d", NDV, ba, bs, bm, bd);
    if (first >= 0) { du_t x = { .u = xa[first] }, y = { .u = xb[first] }, r = { x.d + y.d }; printf("     first add: %016llx + %016llx = %016llx want %016llx\n", xa[first], xb[first], r.u, r_add[first]); }
    {   /* fast paths (asm add/sub/mul, C div) vs the general code, 200 000 random pairs each */
        double ndf_dadd_ref(double, double), ndf_dmul_ref(double, double), ndf_ddiv_ref(double, double);
        int ra = 0, rs = 0, rm = 0, rd = 0;
        for (int i = 0; i < 200000; i++) {
            du_t x, y;
            x.u = (uint64_t)RND() << 32 | RND(); y.u = (uint64_t)RND() << 32 | RND();
            uint32_t k = RND() & 7, ex = 1 + RND() % 2046;
            if (k < 5) {                                     /* exponents close: gaps 0..64, mixed signs */
                int ey = (int)ex - (int)(RND() % 65) * (k & 1 ? 1 : -1);
                if (ey < 1) ey = 1;
                if (ey > 2046) ey = 2046;
                x.u = (x.u & 0x800FFFFFFFFFFFFFull) | (uint64_t)ex << 52;
                y.u = (y.u & 0x800FFFFFFFFFFFFFull) | (uint64_t)ey << 52;
            } else if (k == 5) {                             /* near-equal magnitudes: cancellation */
                y.u = x.u ^ (RND() & 3) ^ (RND() & 1 ? 1ull << 63 : 0);
            } else if (k == 6) {                             /* near the top / bottom of the range */
                x.u = (x.u & 0x800FFFFFFFFFFFFFull) | (uint64_t)(RND() & 1 ? 2046 - RND() % 3 : 1 + RND() % 3) << 52;
                y.u = (y.u & 0x800FFFFFFFFFFFFFull) | (uint64_t)(RND() & 1 ? 2046 - RND() % 3 : 1 + RND() % 3) << 52;
            }
            volatile double vx = x.d, vy = y.d; du_t r, w;
            r.d = vx + vy; w.d = ndf_dadd_ref(vx, vy); if (!dsame(r.u, w.u)) { if (!ra) printf("     add %016llx %016llx -> %016llx want %016llx\n", x.u, y.u, r.u, w.u); ra++; }
            r.d = vx - vy; w.d = ndf_dadd_ref(vx, -vy); if (!dsame(r.u, w.u)) { if (!rs) printf("     sub %016llx %016llx -> %016llx want %016llx\n", x.u, y.u, r.u, w.u); rs++; }
            r.d = vx * vy; w.d = ndf_dmul_ref(vx, vy); if (!dsame(r.u, w.u)) { if (!rm) printf("     mul %016llx %016llx -> %016llx want %016llx\n", x.u, y.u, r.u, w.u); rm++; }
            r.d = vx / vy; w.d = ndf_ddiv_ref(vx, vy); if (!dsame(r.u, w.u)) { if (!rd) printf("     div %016llx %016llx -> %016llx want %016llx\n", x.u, y.u, r.u, w.u); rd++; }
        }
        CHECK(!(ra | rs | rm | rd), "fast paths vs general code, 200 000 random pairs: mismatches %d %d %d %d", ra, rs, rm, rd);
    }
    CHECK(!bc, "double compares (< > == <= >= != unordered): %d mismatches", bc);
    bad = 0;
    for (int i = 0; i < 400; i++) {
        volatile int64_t v = il[i]; du_t d = { (double)v };
        if (d.u != il_d[i]) bad++;
        if (d.d < 9.2233720368547758e18 && d.d > -9.2233720368547758e18) { volatile double vd = d.d; if ((double)(int64_t)vd != vd) bad++; }
    }
    { volatile double v1 = -2.7, v2 = 3.99, v3 = 4e9, v4 = -1e300; volatile int32_t i32 = -123456;
      if ((int)v1 != -2 || (unsigned)v2 != 3 || (unsigned)v3 != 4000000000u || (int)v4 != INT32_MIN || (double)i32 != -123456.0) bad++; }
    CHECK(!bad, "int <-> double conversions: %d mismatches", bad);
    { volatile double x = 1.2345678901234, y = 6.789012345678; double r = 0;
      uint32_t c0 = ccount(); for (int i = 0; i < 100; i++) r += x * y; uint32_t cm = (ccount() - c0) / 100;
      c0 = ccount(); for (int i = 0; i < 100; i++) r += x / y; uint32_t cd = (ccount() - c0) / 100;
      c0 = ccount(); for (int i = 0; i < 100; i++) r = r + x; uint32_t ca = (ccount() - c0) / 100;
      printf("     double add %lu, mul %lu, div %lu cycles (r=%d)\n", (unsigned long)ca, (unsigned long)(cm - ca), (unsigned long)(cd - ca), (int)r); }

    static char buf[1200]; bad = 0;
    for (unsigned i = 0; i < sizeof fcase / sizeof fcase[0]; i++) {
        snprintf(buf, sizeof buf, fcase[i].fmt, fcase[i].v);
        if (strcmp(buf, fcase[i].want)) { bad++; printf("     \"%s\": got \"%s\" want \"%s\"\n", fcase[i].fmt, buf, fcase[i].want); }
    }
    CHECK(!bad, "printf floats: %u/%u formats match C", (unsigned)(sizeof fcase / sizeof fcase[0]) - bad, (unsigned)(sizeof fcase / sizeof fcase[0]));
    snprintf(buf, sizeof buf, "[%5d|%-5d|%05d|%+d|% d|%.3d|%x|%08lX|%s|%.2s|%c|%lld|%llu|%%]", 42, 42, -42, 7, 7, 5, 255u, 0xBEEFul, "hi", "abcdef", 'z', -9000000000ll, 18000000000000000000ull);
    CHECK(!strcmp(buf, "[   42|42   |-0042|+7| 7|005|ff|0000BEEF|hi|ab|z|-9000000000|18000000000000000000|%]"), "printf ints/strings: %s", buf);
    {   static const struct { const char *want; int which; } hc[] = {
            { "0x1p+0", 0 }, { "0x1p-1", 1 }, { "0x1.8p+1", 2 }, { "-0x1.999999999999ap-4", 3 }, { "0x1.555p-2", 4 },
            { "0x2p+0", 5 }, { "0x0.0000000000001p-1022", 6 }, { "0x0p+0", 7 }, { "0X1.FFP+7", 8 }, { "0x1.00000p+0", 9 } };
        bad = 0;
        for (unsigned i = 0; i < sizeof hc / sizeof hc[0]; i++) {
            switch (hc[i].which) {
            case 0: snprintf(buf, sizeof buf, "%a", 1.0); break;
            case 1: snprintf(buf, sizeof buf, "%a", 0.5); break;
            case 2: snprintf(buf, sizeof buf, "%a", 3.0); break;
            case 3: snprintf(buf, sizeof buf, "%a", -0.1); break;
            case 4: snprintf(buf, sizeof buf, "%.3a", 1.0 / 3); break;
            case 5: snprintf(buf, sizeof buf, "%.0a", 1.5); break;
            case 6: snprintf(buf, sizeof buf, "%a", 4.9406564584124654e-324); break;
            case 7: snprintf(buf, sizeof buf, "%a", 0.0); break;
            case 8: snprintf(buf, sizeof buf, "%A", 255.5); break;
            case 9: snprintf(buf, sizeof buf, "%.5a", 1.0); break;
            }
            if (strcmp(buf, hc[i].want)) { bad++; printf("     %%a case %u: got \"%s\" want \"%s\"\n", i, buf, hc[i].want); }
        }
        CHECK(!bad, "%%a hex floats: %u cases", (unsigned)(sizeof hc / sizeof hc[0]));
        int cnt = 0; short scnt = 0;
        snprintf(buf, sizeof buf, "[%#x|%#X|%#o|%o|%#x|%#010x|%hhd|%hd|%hhu|%jd|%p|%5o]%n%hn", 255u, 255u, 8u, 8u, 0u, 255u, 300, 70000, 257,
                 (intmax_t)1 << 40, (void *)0x1234, 8u, &cnt, &scnt);
        CHECK(!strcmp(buf, "[0xff|0XFF|010|10|0|0x000000ff|44|4464|1|1099511627776|0x1234|   10]") && cnt == 68 && scnt == 68,
              "printf %%#x %%o %%hh %%h %%j %%p %%n: %s (n=%d)", buf, cnt);
    }
    int n = snprintf(buf, 8, "%s", "truncate-me");
    CHECK(n == 11 && !strcmp(buf, "truncat"), "snprintf truncates, returns full length (%d)", n);
    float pf = 1.5f; snprintf(buf, sizeof buf, "%.3f", pf);
    CHECK(!strcmp(buf, "1.500"), "float argument promoted: %s", buf);

    char *e;
    CHECK(strtol("  -1234xyz", &e, 10) == -1234 && *e == 'x', "strtol decimal + end pointer");
    CHECK(strtol("0x7fffffff", 0, 0) == 0x7fffffff && strtol("0x80000000", 0, 0) == LONG_MAX && strtol("-0x80000001", 0, 16) == LONG_MIN, "strtol hex + saturation");
    CHECK(strtoul("0777", 0, 0) == 0777 && strtoul("z", &e, 36) == 35 && strtoull("18446744073709551615", 0, 10) == ULLONG_MAX, "strtoul octal/base36, strtoull max");
    CHECK(strtoll("-9223372036854775808", 0, 10) == LLONG_MIN && atoi("  +56") == 56, "strtoll min, atoi");
    { int bd = 0, bf = 0, be = 0;
      for (int i = 0; i < NSTR; i++) {
          char *end; du_t d = { strtod(sv[i], &end) }; if (*end) be++;
          fu_t f = { strtof(sv[i], &end) }; if (*end) be++;
          if (!dsame(d.u, sv_d[i])) { if (!bd) printf("     strtod(\"%.60s\") = %016llx want %016llx\n", sv[i], d.u, sv_d[i]); bd++; }
          if (!same(f.u, sv_f[i])) { if (!bf) printf("     strtof(\"%.60s\") = %08lx want %08lx\n", sv[i], f.u, sv_f[i]); bf++; }
      }
      CHECK(!bd && !bf && !be, "strtod / strtof correctly rounded: %d strings (hard cases, halfway + 800-digit tails, hex, denormals), mismatches %d %d, end %d",
            NSTR, bd, bf, be); }

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
    dbench();
    {   uint32_t t0 = ccount(); for (int i = 0; i < 10; i++) snprintf(buf, sizeof buf, "%.17g", 1.2345678901234567e300); uint32_t a1 = (ccount() - t0) / 10;
        t0 = ccount(); for (int i = 0; i < 10; i++) snprintf(buf, sizeof buf, "%.17g", 4.9406564584124654e-324); uint32_t a2 = (ccount() - t0) / 10;
        t0 = ccount(); for (int i = 0; i < 10; i++) (void)strtod("3.141592653589793", 0); uint32_t a3 = (ccount() - t0) / 10;
        t0 = ccount(); for (int i = 0; i < 10; i++) (void)strtod("2.2250738585072011e-308", 0); uint32_t a4 = (ccount() - t0) / 10;
        printf("     snprintf %%.17g 1.23e300 %lu, denormal %lu, strtod 16 digits %lu, hard case %lu cycles\n",
               (unsigned long)a1, (unsigned long)a2, (unsigned long)a3, (unsigned long)a4); }
    {   /* calendar time vs Python time.gmtime: gmtime_r and timegm round trip over 0001..9999 */
        int bad = 0;
        for (unsigned i = 0; i < sizeof tv_ref / sizeof tv_ref[0]; i++) {
            time_t t = (time_t)tv_ref[i].t; struct tm m;
            gmtime_r(&t, &m);
            bad += m.tm_year != tv_ref[i].y || m.tm_mon != tv_ref[i].mo || m.tm_mday != tv_ref[i].d || m.tm_hour != tv_ref[i].h ||
                   m.tm_min != tv_ref[i].mi || m.tm_sec != tv_ref[i].s || m.tm_wday != tv_ref[i].wd || m.tm_yday != tv_ref[i].yd;
            bad += timegm(&m) != t;
        }
        CHECK(bad == 0, "gmtime_r / timegm: %u host vectors (year 1..9999, negative times), %d wrong", (unsigned)(sizeof tv_ref / sizeof tv_ref[0]), bad);
        struct tm n = { .tm_year = 124, .tm_mon = 13, .tm_mday = 32, .tm_hour = 25 };   /* 2024-14-32 25:00 -> 2025-03-05 01:00 */
        time_t nt = mktime(&n);
        char b[64]; strftime(b, sizeof b, "%F %T %a %b %j", &n);
        CHECK(!strcmp(b, "2025-03-05 01:00:00 Wed Mar 064") && nt == 1741136400, "mktime normalises + strftime: %s (%lld)", b, (long long)nt);
        struct timeval tv = { .tv_sec = 1761350400 }; settimeofday(&tv, 0);
        (void)time(0); uint32_t c0 = ccount(); time_t now = time(0); uint32_t tc = ccount() - c0;
        struct timeval g; gettimeofday(&g, 0);
        CHECK(now == 1761350400 && g.tv_sec == 1761350400, "settimeofday / time / gettimeofday: %lld, time() %lu cycles", (long long)now, (unsigned long)tc);
        c0 = ccount(); struct tm m; gmtime_r(&now, &m); uint32_t gc = ccount() - c0;
        c0 = ccount(); struct tm mm = m; (void)mktime(&mm); uint32_t mc = ccount() - c0;
        c0 = ccount(); strftime(b, sizeof b, "%F %T", &m); uint32_t sc2 = ccount() - c0;
        printf("     gmtime_r %lu, mktime %lu, strftime %%F %%T %lu cycles\n", (unsigned long)gc, (unsigned long)mc, (unsigned long)sc2);
    }
    printf("=== %s (%d failures) ===\n", fails ? "FAILED" : "ALL PASS", fails);
    for (;;) {}
}
