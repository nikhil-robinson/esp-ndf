/* =============================================================================
 * string.h  –  Minimal string / memory functions for bare-metal ESP32-S3
 *
 * This header replaces <string.h> from the standard library.  It covers
 * only the functions commonly required in embedded firmware:
 *   Memory: memset, memcpy, memmove, memcmp, memchr
 *   String: strlen, strcpy, strncpy, strcmp, strncmp, strstr, strchr
 *
 * Note: For maximum performance on Xtensa LX7, the ROM provides hardware-
 *   accelerated variants (see arch/xtensa/ld/esp32s3.rom.ld).  If you
 *   declare these as external symbols and link against the ROM map, the
 *   linker will use the ROM versions automatically.
 * ============================================================================= */
#ifndef NDF_STRING_H
#define NDF_STRING_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Memory functions --------------------------------------------------- */

/**
 * @brief  Fill a memory region with a constant byte.
 * @param  s    Destination pointer
 * @param  c    Fill byte (interpreted as unsigned char)
 * @param  n    Number of bytes to fill
 * @return s
 */
void *memset(void *s, int c, size_t n);

/**
 * @brief  Copy n bytes from src to dst (regions must NOT overlap).
 * @return dst
 */
void *memcpy(void *dst, const void *src, size_t n);

/**
 * @brief  Copy n bytes from src to dst (regions MAY overlap).
 * @return dst
 */
void *memmove(void *dst, const void *src, size_t n);

/**
 * @brief  Compare n bytes of memory.
 * @return <0, 0, or >0
 */
int memcmp(const void *a, const void *b, size_t n);

/**
 * @brief  Search for a byte value in a memory region.
 * @return Pointer to first occurrence of c in s, or NULL.
 */
void *memchr(const void *s, int c, size_t n);

/* ---- String functions --------------------------------------------------- */

/**
 * @brief  Return the length of a null-terminated string (not counting '\0').
 */
size_t strlen(const char *s);

/**
 * @brief  Copy string src to dst (including '\0').  dst must be large enough.
 * @return dst
 */
char *strcpy(char *dst, const char *src);

/**
 * @brief  Copy at most n-1 characters of src to dst; always null-terminates.
 * @return dst
 */
char *strncpy(char *dst, const char *src, size_t n);

/**
 * @brief  Lexicographic comparison of two null-terminated strings.
 * @return <0, 0, or >0
 */
int strcmp(const char *a, const char *b);

/**
 * @brief  Like strcmp but compare at most n characters.
 */
int strncmp(const char *a, const char *b, size_t n);

/**
 * @brief  Find first occurrence of c in string s.
 * @return Pointer to the character, or NULL.
 */
char *strchr(const char *s, int c);

/**
 * @brief  Find first occurrence of substring needle in haystack.
 * @return Pointer to the start of needle within haystack, or NULL.
 */
char *strstr(const char *haystack, const char *needle);

#ifdef __cplusplus
}
#endif

#endif /* NDF_STRING_H */
