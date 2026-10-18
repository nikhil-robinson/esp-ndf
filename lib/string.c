/* mem* functions the compiler may call implicitly (struct copies/inits).
 * Word-at-a-time when both pointers are aligned, bytes otherwise. */
#include <stddef.h>
#include <stdint.h>

void dsp_copy128(void *dst, const void *src, size_t n);
void dsp_set128(void *dst, const void *pattern16, size_t n);
void dsp_copy128u(void *dst, const void *src, size_t n);

/* 128-bit vector path: internal DRAM only, and only when the PIE unit is on
 * (it is off inside interrupt handlers, which may also call memcpy) */
static inline int vec_ok(uintptr_t a, uintptr_t b, size_t n)
{
    uint32_t cp;
    __asm__ volatile("rsr.cpenable %0" : "=r"(cp));
    return n >= 64 && (cp & 8) && a >= 0x3FC88000u && a + n <= 0x3FD00000u &&
           b >= 0x3FC88000u && b + n + 16 <= 0x3FD00000u;          /* +16: unaligned copy over-reads one block */
}

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d; const uint8_t *sp = s;
    if (((uintptr_t)dp & 15) == ((uintptr_t)sp & 15) && vec_ok((uintptr_t)dp, (uintptr_t)sp, n)) {
        while ((uintptr_t)dp & 15) { *dp++ = *sp++; n--; }      /* both reach 16-byte alignment together */
        size_t v = n & ~(size_t)31;
        dsp_copy128(dp, sp, v);
        dp += v; sp += v; n -= v;
    } else if (vec_ok((uintptr_t)dp, (uintptr_t)sp, n)) {           /* different alignments */
        while ((uintptr_t)dp & 15) { *dp++ = *sp++; n--; }
        size_t v = n & ~(size_t)31;
        dsp_copy128u(dp, sp, v);
        dp += v; sp += v; n -= v;
    } else if ((((uintptr_t)dp | (uintptr_t)sp) & 3) == 0) {
        for (; n >= 16; n -= 16, dp += 16, sp += 16) {
            uint32_t a = ((const uint32_t *)sp)[0], b = ((const uint32_t *)sp)[1];
            uint32_t c = ((const uint32_t *)sp)[2], e = ((const uint32_t *)sp)[3];
            ((uint32_t *)dp)[0] = a; ((uint32_t *)dp)[1] = b; ((uint32_t *)dp)[2] = c; ((uint32_t *)dp)[3] = e;
        }
    }
    if ((((uintptr_t)dp | (uintptr_t)sp) & 3) == 0)
        for (; n >= 4; n -= 4, dp += 4, sp += 4) *(uint32_t *)dp = *(const uint32_t *)sp;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d; const uint8_t *sp = s;
    if (dp <= sp || dp >= sp + n) return memcpy(d, s, n);
    while (n--) dp[n] = sp[n];                    /* overlap, copy backwards */
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    uint32_t w = (uint8_t)c * 0x01010101u;
    while (n && ((uintptr_t)dp & 15)) { *dp++ = c; n--; }
    if (vec_ok((uintptr_t)dp, (uintptr_t)dp, n)) {
        uint32_t pat[4] __attribute__((aligned(16))) = { w, w, w, w };
        size_t v = n & ~(size_t)31;
        dsp_set128(dp, pat, v);
        dp += v; n -= v;
    }
    for (; n >= 4; n -= 4, dp += 4) *(uint32_t *)dp = w;
    while (n--) *dp++ = c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return p - s;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (uint8_t)*a - (uint8_t)*b;
        if (!*a) return 0;
    }
    return 0;
}

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c ? 0 : (char *)s;
}

char *strrchr(const char *s, int c)
{
    const char *r = 0;
    for (; *s; s++) if (*s == (char)c) r = s;
    return c ? (char *)r : (char *)s;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}
