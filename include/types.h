/**
 * @file types.h
 * @brief Common type definitions for the esp-ndf bare-metal SDK.
 *
 * Provides stdint-style fixed-width types, bool, NULL, size_t and a set of
 * convenience integer aliases. This header must be self-contained (no libc).
 */
#ifndef NDF_TYPES_H
#define NDF_TYPES_H

/* ---- Fixed-width integers (matches C99 <stdint.h>) ---------------------- */
typedef unsigned char      uint8_t;
typedef signed   char      int8_t;
typedef unsigned short     uint16_t;
typedef signed   short     int16_t;
typedef unsigned int       uint32_t;
typedef signed   int       int32_t;
typedef unsigned long long uint64_t;
typedef signed   long long int64_t;

/* ---- Pointer-size integer ----------------------------------------------- */
typedef uint32_t  uintptr_t;
typedef int32_t   intptr_t;
typedef uint32_t  size_t;
typedef int32_t   ssize_t;
typedef int32_t   ptrdiff_t;

/* ---- Register access type (always 32-bit on Xtensa) --------------------- */
typedef volatile uint32_t  reg32_t;

/* ---- Boolean ------------------------------------------------------------ */
#ifndef __cplusplus
typedef uint8_t  bool;
#define true     ((bool)1)
#define false    ((bool)0)
#endif

/* ---- NULL --------------------------------------------------------------- */
#ifndef NULL
#  define NULL ((void *)0)
#endif

/* ---- Min / Max ---------------------------------------------------------- */
#ifndef MIN
#  define MIN(a, b)  (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#  define MAX(a, b)  (((a) > (b)) ? (a) : (b))
#endif

/* ---- Array length ------------------------------------------------------- */
#define ARRAY_SIZE(x)  (sizeof(x) / sizeof((x)[0]))

/* ---- Bit manipulation --------------------------------------------------- */
#define BIT(n)          (1u << (n))
#define BIT64(n)        (1ull << (n))
#define BITS(h, l)      (((1u << ((h) - (l) + 1)) - 1) << (l))
#define GET_BITS(v,h,l) (((v) >> (l)) & ((1u << ((h)-(l)+1))-1))

#endif /* NDF_TYPES_H */
