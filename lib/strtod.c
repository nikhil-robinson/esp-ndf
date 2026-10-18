/* Correctly rounded decimal (and hex) to binary: strtod, strtof, atof.
 *
 * 1. Exact fast path: <= 2^p significand and |exp10| small -> one correctly
 *    rounded multiply or divide (Clinger).
 * 2. Otherwise a 64-bit approximation D * 10^e from a table of normalised
 *    powers of ten (two 64x64 products, error < 32 units of its last bit).
 *    If the bits below the rounding point are not within that error of the
 *    halfway point, the rounding is already certain.
 * 3. Near a halfway point: exact big-integer comparison of the decimal value
 *    (all digits, up to 800 significant, the rest folded into a sticky flag)
 *    against the halfway point.
 * The same code serves double (p = 53) and float (p = 24), so strtof rounds
 * the decimal directly (no double rounding through strtod). */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "bigint.h"
#include "pow10_table.h"

#define MAXDIG 800
#define ERR    32

typedef struct { int p, bias, emin, fmax; } fmt_t;   /* precision, bias, smallest normal exponent, max field */
static const fmt_t F64 = { 53, 1023, -1022, 2047 }, F32 = { 24, 127, -126, 255 };

static uint64_t mulhi(uint64_t a, uint64_t b, uint64_t *lo)
{
    uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    *lo = mid << 32 | (uint32_t)ll;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

/* packs significand m (implicit bit at p-1 when normal) with exponent L of
 * that bit; L < emin means m is already scaled to the denormal grid */
static uint64_t pack(const fmt_t *f, uint64_t sign, uint64_t m, int L)
{
    if (!m) return sign;
    if (m >> f->p) { m >>= 1; L++; }                       /* rounding carried to 2^p */
    int field = L < f->emin ? 0 : L + f->bias;
    if (field >= f->fmax) return sign | (uint64_t)f->fmax << (f->p - 1);   /* inf */
    uint64_t bits = field ? ((uint64_t)(field - 1) << (f->p - 1)) + m : m;  /* implicit bit carries in */
    return sign | bits;
}

/* round x * 2^bexp (x != 0, any normalisation) to format f; sticky = more bits below x */
static uint64_t round_exact(const fmt_t *f, uint64_t sign, uint64_t x, int bexp, bool sticky)
{
    int lz = __builtin_clzll(x);
    x <<= lz; bexp -= lz;
    int L = bexp + 63, k = 64 - f->p;
    if (L < f->emin) k += f->emin - L;
    if (k > 64) return sign;                               /* below half the smallest denormal */
    uint64_t keep, rem, half;
    if (k == 64) { keep = 0; rem = x; half = 1ull << 63; }
    else { keep = x >> k; rem = x & ((1ull << k) - 1); half = 1ull << (k - 1); }
    if (rem > half || (rem == half && (sticky || (keep & 1)))) keep++;
    return pack(f, sign, keep, L < f->emin ? f->emin - 1 : L);
}

static int isspace_(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

static uint64_t parse_hex(const fmt_t *f, uint64_t sign, const char *p, char **end)
{
    uint64_t m = 0; int e = 0; bool any = false, sticky = false, dot = false;
    for (;; p++) {
        if (*p == '.' && !dot) { dot = true; continue; }
        int h = hexval(*p);
        if (h < 0) break;
        any = true;
        if (m >> 60) { sticky |= h != 0; if (!dot) e += 4; }
        else { m = m << 4 | (uint64_t)h; if (dot) e -= 4; }
    }
    if (!any) return ~0ull;
    if ((*p | 32) == 'p') {
        const char *q = p + 1; bool en = false; int ev = 0;
        if (*q == '+' || *q == '-') en = *q++ == '-';
        if (*q >= '0' && *q <= '9') { for (; *q >= '0' && *q <= '9'; q++) if (ev < 100000) ev = ev * 10 + (*q - '0'); p = q; e += en ? -ev : ev; }
    }
    if (end) *end = (char *)p;
    return m ? round_exact(f, sign, m, e, sticky) : sign;
}

static uint64_t convert(const fmt_t *f, const char *s, char **end)
{
    const char *p = s;
    while (isspace_(*p)) p++;
    uint64_t sign = 0;
    if (*p == '+' || *p == '-') { if (*p == '-') sign = 1ull << (f->p == 53 ? 63 : 31); p++; }
    uint64_t inf = sign | (uint64_t)f->fmax << (f->p - 1);
    if ((p[0] | 32) == 'i' && (p[1] | 32) == 'n' && (p[2] | 32) == 'f') {
        p += 3;
        static const char inity[] = "inity";
        int k = 0; while (k < 5 && (p[k] | 32) == inity[k]) k++;
        if (k == 5) p += 5;
        if (end) *end = (char *)p;
        return inf;
    }
    if ((p[0] | 32) == 'n' && (p[1] | 32) == 'a' && (p[2] | 32) == 'n') {
        p += 3;
        if (*p == '(') { const char *q = p + 1; while ((*q >= '0' && *q <= '9') || ((*q | 32) >= 'a' && (*q | 32) <= 'z') || *q == '_') q++; if (*q == ')') p = q + 1; }
        if (end) *end = (char *)p;
        return inf | 1ull << (f->p - 2);                    /* quiet NaN */
    }
    if (p[0] == '0' && (p[1] | 32) == 'x') {
        uint64_t r = parse_hex(f, sign, p + 2, end);
        if (r != ~0ull) return r;
        if (end) *end = (char *)p + 1;                      /* "0x" with no digits: the "0" */
        return sign;
    }

    /* decimal: significant digits (leading zeros skipped), value = digits * 10^dexp */
    const char *dig0 = 0; int nd = 0, dexp = 0; bool any = false, dot = false, tail = false;
    uint64_t d19 = 0;
    for (;; p++) {
        if (*p == '.' && !dot) { dot = true; continue; }
        if (*p < '0' || *p > '9') break;
        any = true;
        if (!nd && *p == '0') { if (dot) dexp--; continue; }
        if (!dig0) dig0 = p;
        if (nd < MAXDIG) { if (nd < 19) d19 = d19 * 10 + (uint64_t)(*p - '0'); nd++; if (dot) dexp--; }
        else { tail |= *p != '0'; if (!dot) dexp++; }
    }
    if (!any) { if (end) *end = (char *)s; return 0; }
    if ((*p | 32) == 'e') {
        const char *q = p + 1; bool en = false; int ev = 0;
        if (*q == '+' || *q == '-') en = *q++ == '-';
        if (*q >= '0' && *q <= '9') { for (; *q >= '0' && *q <= '9'; q++) if (ev < 100000) ev = ev * 10 + (*q - '0'); p = q; dexp += en ? -ev : ev; }
    }
    if (end) *end = (char *)p;
    if (!nd) return sign;
    int mag = nd + dexp;                                    /* value in [10^(mag-1), 10^mag) */
    if (mag > (f->p == 53 ? 310 : 40)) return inf;
    if (mag < (f->p == 53 ? -326 : -47)) return sign;

    /* 1. exact fast path */
    if (nd <= 19 && !tail && d19 < (1ull << f->p) && dexp >= -22 && dexp <= 22) {
        if (f->p == 53) {
            static const double e[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                        1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
            double v = (double)d19;
            v = dexp >= 0 ? v * e[dexp] : v / e[-dexp];
            union { double d; uint64_t u; } r = { v };
            return r.u | sign;
        } else if (dexp >= -10 && dexp <= 10) {
            static const float e[] = { 1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f };
            float v = (float)(uint32_t)d19;
            v = dexp >= 0 ? v * e[dexp] : v / e[-dexp];
            union { float f; uint32_t u; } r = { v };
            return r.u | sign;
        }
    }

    /* 2. approximation from the first 19 digits */
    int e10 = dexp + (nd > 19 ? nd - 19 : 0);
    int j = e10 >= 0 ? e10 / P10_STEP : -((-e10 + P10_STEP - 1) / P10_STEP), r = e10 - j * P10_STEP;
    int lz = __builtin_clzll(d19);
    uint64_t lo, x = mulhi(d19 << lz, p10_small[r].m, &lo);
    int bexp = 64 - lz + p10_small[r].e;
    if (!(x >> 63)) { x = x << 1 | lo >> 63; bexp--; }
    if (j) {
        x = mulhi(x, p10_large[j - P10_LMIN].m, &lo);
        bexp += 64 + p10_large[j - P10_LMIN].e;
        if (!(x >> 63)) { x = x << 1 | lo >> 63; bexp--; }
    }
    int L = bexp + 63, k = 64 - f->p;
    if (L < f->emin) k += f->emin - L;
    if (k <= 64 - 6) {                                      /* enough bits below the rounding point to judge */
        uint64_t rem = k == 64 ? x : x & ((1ull << k) - 1), half = 1ull << (k - 1);
        uint64_t dist = rem > half ? rem - half : half - rem;
        if (dist > ERR) return round_exact(f, sign, x, bexp, false);   /* far from halfway: x decides */
    }

    /* 3. near a halfway point (or deep denormal): exact comparison */
    uint64_t keep = k >= 64 ? 0 : x >> k;
    int t = bexp + k - 1;                                   /* halfway = (2 keep + 1) * 2^t */
    big_t A, B;
    A.n = 0;
    {   /* A = all kept digits */
        const char *q = dig0; int left = nd; uint32_t chunk = 0, cn = 0;
        static const uint32_t p10[] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 };
        for (; left; q++) {
            if (*q == '.') continue;
            chunk = chunk * 10 + (uint32_t)(*q - '0'); cn++; left--;
            if (cn == 9 || !left) { big_mul_small(&A, p10[cn]); if (!A.n) big_set_u64(&A, chunk); else big_add_small(&A, chunk); chunk = 0; cn = 0; }
        }
    }
    big_set_u64(&B, 2 * keep + 1);
    if (dexp >= 0) big_mul_pow10(&A, dexp); else big_mul_pow10(&B, -dexp);
    if (t >= 0) big_shl(&B, t); else big_shl(&A, -t);
    int c = big_cmp(&A, &B);
    if (!c && tail) c = 1;
    if (c > 0 || (!c && (keep & 1))) keep++;
    return pack(f, sign, keep, L < f->emin ? f->emin - 1 : L);
}

double strtod(const char *s, char **end)
{
    union { double d; uint64_t u; } r = { .u = convert(&F64, s, end) };
    return r.d;
}

float strtof(const char *s, char **end)
{
    union { float f; uint32_t u; } r = { .u = (uint32_t)convert(&F32, s, end) };
    return r.f;
}

double atof(const char *s) { return strtod(s, 0); }
