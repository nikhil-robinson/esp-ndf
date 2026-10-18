/* stdlib subset: strtol family, atoi/atol, strtof/strtod, qsort, bsearch, abs.
 * strtof: exact when the digits fit 24 bits and |exp10| <= 10 (the common
 * case), else within 1 ulp. strtod: float precision.
 * ponytail: correctly rounded strtod needs big-integer scaling; add if
 * double parsing of long literals matters. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>

static int isspace_(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int digit_(int c, int base)
{
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
    return d < base ? d : -1;
}

/* parse magnitude; *neg set from sign; *ovf on overflow of `max` */
static uint64_t parse_u(const char *s, char **end, int base, bool *neg, uint64_t max, bool *ovf)
{
    const char *p = s;
    while (isspace_(*p)) p++;
    *neg = false; *ovf = false;
    if (*p == '+' || *p == '-') *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_(p[2], 16) >= 0) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    if (base < 2 || base > 36) { if (end) *end = (char *)s; return 0; }
    uint64_t v = 0; const char *start = p; int d;
    while ((d = digit_(*p, base)) >= 0) {
        if (v > (max - (uint64_t)d) / (uint64_t)base) *ovf = true; else v = v * (uint64_t)base + (uint64_t)d;
        p++;
    }
    if (end) *end = (char *)(p == start ? s : p);
    return v;
}

long long strtoll(const char *s, char **end, int base)
{
    bool neg, ovf; uint64_t v = parse_u(s, end, base, &neg, (uint64_t)LLONG_MAX + 1, &ovf);
    if (ovf || (!neg && v > (uint64_t)LLONG_MAX)) return neg ? LLONG_MIN : LLONG_MAX;
    return neg ? (long long)(0 - v) : (long long)v;
}
unsigned long long strtoull(const char *s, char **end, int base)
{
    bool neg, ovf; uint64_t v = parse_u(s, end, base, &neg, ULLONG_MAX, &ovf);
    if (ovf) return ULLONG_MAX;
    return neg ? 0 - v : v;
}
long strtol(const char *s, char **end, int base)
{
    bool neg, ovf; uint64_t v = parse_u(s, end, base, &neg, (uint64_t)LONG_MAX + 1, &ovf);
    if (ovf || (!neg && v > (uint64_t)LONG_MAX)) return neg ? LONG_MIN : LONG_MAX;
    return neg ? (long)(0 - v) : (long)v;
}
unsigned long strtoul(const char *s, char **end, int base)
{
    bool neg, ovf; uint64_t v = parse_u(s, end, base, &neg, ULONG_MAX, &ovf);
    if (ovf) return ULONG_MAX;
    return neg ? (unsigned long)(0 - v) : (unsigned long)v;
}
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
long long atoll(const char *s) { return strtoll(s, 0, 10); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

static const float p10[] = { 1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f };

float strtof(const char *s, char **end)
{
    const char *p = s;
    while (isspace_(*p)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    union { float f; uint32_t u; } r;
    if ((p[0] | 32) == 'i' && (p[1] | 32) == 'n' && (p[2] | 32) == 'f') {
        p += 3; if (!strncmp(p, "inity", 5) || !strncmp(p, "INITY", 5)) p += 5;
        r.u = 0x7F800000u | (neg ? 0x80000000u : 0); if (end) *end = (char *)p; return r.f;
    }
    if ((p[0] | 32) == 'n' && (p[1] | 32) == 'a' && (p[2] | 32) == 'n') { r.u = 0x7FC00000u; if (end) *end = (char *)p + 3; return r.f; }
    uint64_t m = 0; int e10 = 0, nd = 0; bool any = false;
    for (; *p >= '0' && *p <= '9'; p++) { any = true; if (nd < 19) { m = m * 10 + (uint64_t)(*p - '0'); if (m) nd++; } else e10++; }
    if (*p == '.') for (p++; *p >= '0' && *p <= '9'; p++) { any = true; if (nd < 19) { m = m * 10 + (uint64_t)(*p - '0'); if (m) nd++; e10--; } }
    if (!any) { if (end) *end = (char *)s; return 0.0f; }
    if ((*p | 32) == 'e') {
        const char *q = p + 1; bool en = false; int ev = 0;
        if (*q == '+' || *q == '-') en = *q++ == '-';
        if (*q >= '0' && *q <= '9') { for (; *q >= '0' && *q <= '9'; q++) if (ev < 10000) ev = ev * 10 + (*q - '0'); p = q; e10 += en ? -ev : ev; }
    }
    if (end) *end = (char *)p;
    float f = (float)m;                                          /* one rounding (exact below 2^24) */
    if (m && e10 > 0) { while (e10 > 10) { f *= 1e10f; e10 -= 10; } f *= p10[e10]; }
    else if (m && e10 < 0) {
        while (e10 < -10) { f /= 1e10f; e10 += 10; if (f == 0.0f) break; }
        f /= p10[-e10];
    }
    return neg ? -f : f;
}
double strtod(const char *s, char **end) { return strtof(s, end); }
double atof(const char *s) { return strtof(s, 0); }

/* qsort: median-of-3 quicksort, insertion sort below 12 elements, recursion
 * on the smaller side only (stack depth O(log n)), word-swap fast path.
 * ponytail: adversarial input can still make it O(n^2) time; heapsort
 * fallback (introsort) if that ever matters. */
static void swap_(char *a, char *b, size_t w)
{
    if (!(((uintptr_t)a | (uintptr_t)b | w) & 3)) for (size_t i = 0; i < w; i += 4) { uint32_t t = *(uint32_t *)(a + i); *(uint32_t *)(a + i) = *(uint32_t *)(b + i); *(uint32_t *)(b + i) = t; }
    else for (size_t i = 0; i < w; i++) { char t = a[i]; a[i] = b[i]; b[i] = t; }
}

/* one element-size-specialised body: w == 4 (ints, floats, pointers) swaps
 * one word inline, the rest go through swap_ */
#define QSORT_BODY(SWAP)                                                                  \
    while (n > 12) {                                                                      \
        char *lo = b, *mid = b + (n / 2) * w, *hi = b + (n - 1) * w;                      \
        if (cmp(mid, lo) < 0) SWAP(mid, lo);                                              \
        if (cmp(hi, mid) < 0) { SWAP(hi, mid); if (cmp(mid, lo) < 0) SWAP(mid, lo); }     \
        SWAP(mid, b);                                    /* pivot at b */                 \
        char *i = b + w, *j = hi;                                                         \
        for (;;) {                                                                        \
            while (i <= j && cmp(i, b) < 0) i += w;                                       \
            while (i <= j && cmp(j, b) > 0) j -= w;                                       \
            if (i >= j) break;                                                            \
            SWAP(i, j); i += w; j -= w;                                                   \
        }                                                                                 \
        SWAP(b, j);                                      /* pivot to its place */         \
        size_t nl = (size_t)(j - b) / w, nr = n - nl - 1;                                 \
        if (nl < nr) { qsort(b, nl, w, cmp); b = j + w; n = nr; }                         \
        else { qsort(j + w, nr, w, cmp); n = nl; }                                        \
    }                                                                                     \
    for (size_t k = 1; k < n; k++)                                                        \
        for (char *p = b + k * w; p > b && cmp(p - w, p) > 0; p -= w) SWAP(p - w, p);

#define SWAP4(x, y) do { uint32_t t_ = *(uint32_t *)(x); *(uint32_t *)(x) = *(uint32_t *)(y); *(uint32_t *)(y) = t_; } while (0)
#define SWAPN(x, y) swap_(x, y, w)

void qsort(void *base, size_t n, size_t w, int (*cmp)(const void *, const void *))
{
    char *b = base;
    if (w == 4 && !((uintptr_t)b & 3)) { QSORT_BODY(SWAP4) }
    else { QSORT_BODY(SWAPN) }
}

void *bsearch(const void *key, const void *base, size_t n, size_t w, int (*cmp)(const void *, const void *))
{
    const char *b = base;
    while (n) {
        const char *m = b + (n / 2) * w;
        int c = cmp(key, m);
        if (!c) return (void *)m;
        if (c > 0) { b = m + w; n -= n / 2 + 1; } else n /= 2;
    }
    return 0;
}
