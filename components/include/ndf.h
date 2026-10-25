/* esp-ndf: common types and register access. Everything includes this. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "soc_caps.h"
#include "ndf_config.h"

#define REG(a)            (*(volatile uint32_t *)(uintptr_t)(a))
#define REG_SET(a, m)     (REG(a) |= (m))
#define REG_CLR(a, m)     (REG(a) &= ~(uint32_t)(m))
#define REG_FIELD(a, s, w, v) \
    (REG(a) = (REG(a) & ~(((1u << (w)) - 1) << (s))) | ((uint32_t)(v) << (s)))
#define BIT(n)            (1u << (n))

/* Force a fresh load/store each time (shared with ISRs or the other core). */
#define READ_ONCE(x)      (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v)  (*(volatile __typeof__(x) *)&(x) = (v))

#define RAM_FN   __attribute__((section(".iram.text"), noinline))   /* always in internal RAM */
#define FLASH_FN __attribute__((section(".flash.text"), noinline))  /* in flash (executed through the cache) when NDF_XIP != 0 */
/* RTC slow memory (kept through deep sleep; IDF names): RTC_DATA_ATTR initialised at every boot except a deep-sleep
 * wake, RTC_BSS_ATTR zeroed likewise, RTC_NOINIT_ATTR never touched (kept through any reset but power-on) */
#define RTC_DATA_ATTR   __attribute__((section(".rtc.data")))
#define RTC_BSS_ATTR    __attribute__((section(".rtc.bss")))
#define RTC_NOINIT_ATTR __attribute__((section(".rtc.noinit")))
#define NORETURN __attribute__((noreturn))
#define ALWAYS_INLINE static inline __attribute__((always_inline))
