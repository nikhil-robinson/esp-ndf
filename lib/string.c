/* =============================================================================
 * string.c  –  Bare-metal string / memory library implementation
 *
 * All functions are word-aligned where possible for optimal Xtensa performance.
 * The Xtensa LX7 can do unaligned 32-bit loads/stores but they are slower;
 * aligning the inner loop to 32-bit boundaries gives ~4× speedup.
 * ============================================================================= */
#include "lib/string.h"
#include "compiler.h"

/* ============================================================================
 * memset
 * ============================================================================ */
void *memset(void *s, int c, size_t n)
{
    uint8_t  *p   = (uint8_t *)s;
    uint8_t   val = (uint8_t)(c & 0xFF);
    uint32_t  w;

    /* Handle leading unaligned bytes */
    while (n > 0 && ((uintptr_t)p & 3u)) {
        *p++ = val;
        n--;
    }

    /* Word-fill aligned region */
    if (n >= 4) {
        w  = (uint32_t)val;
        w |= (w << 8);
        w |= (w << 16);   /* replicate byte to all 4 positions */

        uint32_t *wp = (uint32_t *)p;
        size_t    wn = n >> 2;
        while (wn--) {
            *wp++ = w;
        }
        p  = (uint8_t *)wp;
        n &= 3u;
    }

    /* Trailing bytes */
    while (n--) {
        *p++ = val;
    }

    return s;
}

/* ============================================================================
 * memcpy  –  non-overlapping byte copy
 * ============================================================================ */
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    /* If both pointers have the same word alignment, use word copy */
    if (!((uintptr_t)d & 3u) && !((uintptr_t)s & 3u)) {
        uint32_t       *wd = (uint32_t *)d;
        const uint32_t *ws = (const uint32_t *)s;
        size_t          wn = n >> 2;
        while (wn--) {
            *wd++ = *ws++;
        }
        d = (uint8_t *)wd;
        s = (const uint8_t *)ws;
        n &= 3u;
    }

    /* Byte copy for remainder / unaligned case */
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

/* ============================================================================
 * memmove  –  overlapping-safe byte copy
 * ============================================================================ */
void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0) {
        return dst;
    }

    if (d < s || d >= s + n) {
        /* No overlap or dst is before src: forward copy (same as memcpy) */
        return memcpy(dst, src, n);
    }

    /* dst overlaps with the tail of src: copy backward */
    d += n;
    s += n;
    while (n--) {
        *(--d) = *(--s);
    }
    return dst;
}

/* ============================================================================
 * memcmp
 * ============================================================================ */
int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }
        pa++; pb++;
    }
    return 0;
}

/* ============================================================================
 * memchr
 * ============================================================================ */
void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p   = (const uint8_t *)s;
    uint8_t        val = (uint8_t)(c & 0xFF);
    while (n--) {
        if (*p == val) return (void *)p;
        p++;
    }
    return NULL;
}

/* ============================================================================
 * strlen
 * ============================================================================ */
size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

/* ============================================================================
 * strcpy
 * ============================================================================ */
char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

/* ============================================================================
 * strncpy
 * ============================================================================ */
char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n > 0 && *src) {
        *d++ = *src++;
        n--;
    }
    while (n-- > 0) {
        *d++ = '\0';    /* null-pad to n bytes */
    }
    return dst;
}

/* ============================================================================
 * strcmp
 * ============================================================================ */
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* ============================================================================
 * strncmp
 * ============================================================================ */
int strncmp(const char *a, const char *b, size_t n)
{
    while (n > 0 && *a && *a == *b) {
        a++; b++; n--;
    }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* ============================================================================
 * strchr
 * ============================================================================ */
char *strchr(const char *s, int c)
{
    char val = (char)(c & 0xFF);
    while (*s) {
        if (*s == val) return (char *)s;
        s++;
    }
    if (val == '\0') return (char *)s;   /* found the terminator */
    return NULL;
}

/* ============================================================================
 * strstr  –  naive O(n*m) implementation (sufficient for small strings)
 * ============================================================================ */
char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;

    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (*haystack == *needle && memcmp(haystack, needle, nlen) == 0) {
            return (char *)haystack;
        }
    }
    return NULL;
}
