/* host stand-in for arch/xtensa/xtensa.h (fuzz builds): a cycle counter from the host clock, one core */
#pragma once
#include <stdint.h>
#include <time.h>
static inline uint32_t ccount(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 240000000ull + t.tv_nsec * 240ull / 1000); }
static inline int core_id(void) { return 0; }
