#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif
#define LWIP_NO_CTYPE_H 1                   /* our libc has no newlib ctype table: lwIP's own lwip_tolower / lwip_isdigit (DNS name compare) */
#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
void lwip_assert_fail(const char *msg, const char *file, int line);
#define LWIP_PLATFORM_ASSERT(x) lwip_assert_fail(x, __FILE__, __LINE__)
