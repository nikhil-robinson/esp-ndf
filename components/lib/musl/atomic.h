/* esp-ndf shim: fma.c only needs a_clz_64 from musl's atomic.h */
#pragma once
#include <stdint.h>
static inline int a_clz_64(uint64_t x) { return __builtin_clzll(x); }
