/* esp-ndf: common types and register access. Everything includes this. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define REG(a)            (*(volatile uint32_t *)(uintptr_t)(a))
#define REG_SET(a, m)     (REG(a) |= (m))
#define REG_CLR(a, m)     (REG(a) &= ~(uint32_t)(m))
#define REG_FIELD(a, s, w, v) \
    (REG(a) = (REG(a) & ~(((1u << (w)) - 1) << (s))) | ((uint32_t)(v) << (s)))
#define BIT(n)            (1u << (n))

#define RAM_FN   __attribute__((section(".iram.text"), noinline))
#define NORETURN __attribute__((noreturn))
#define ALWAYS_INLINE static inline __attribute__((always_inline))
