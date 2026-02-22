/**
 * @file compiler.h
 * @brief Compiler attribute macros for GCC/Xtensa bare-metal builds.
 *
 * Centralises all __attribute__(...) usage so it is easy to port to
 * other compilers later.
 */
#ifndef NDF_COMPILER_H
#define NDF_COMPILER_H

/* ---- Section placement -------------------------------------------------- */

/** Place function/data in IRAM (fast instruction RAM, TCM-like) */
#define IRAM_ATTR     __attribute__((section(".iram.text")))

/** Place function/data in the RTC fast RAM (survives deep sleep) */
#define RTC_IRAM_ATTR __attribute__((section(".rtc.text")))

/** Place read-only data in DROM (flash-mapped, slow but free) */
#define DROM_ATTR     __attribute__((section(".rodata")))

/** Place variable in RTC slow RAM */
#define RTC_DATA_ATTR __attribute__((section(".rtc.data")))

/** Interrupt service routine function (saves all caller-saved regs) */
#define ISR_ATTR      __attribute__((interrupt))

/* ---- Optimisation hints ------------------------------------------------- */
#define ALWAYS_INLINE __attribute__((always_inline)) static inline
#define NEVER_INLINE  __attribute__((noinline))
#define NORETURN      __attribute__((noreturn))
#define PURE          __attribute__((pure))
#define CONST_FUNC    __attribute__((const))
#define WEAK          __attribute__((weak))
#define ALIAS(sym)    __attribute__((alias(#sym)))
#define USED          __attribute__((used))
#define UNUSED        __attribute__((unused))
#define PACKED        __attribute__((packed))
#define ALIGNED(n)    __attribute__((aligned(n)))
#define NAKED         __attribute__((naked))

/* ---- Likely / Unlikely branch hints ------------------------------------- */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ---- Compile-time assertion ---------------------------------------------- */
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/* ---- Memory / IO barriers ----------------------------------------------- */
/** Full memory barrier (prevents compiler re-ordering across this point) */
#define MEMORY_BARRIER() __asm__ volatile ("" ::: "memory")

/** Xtensa MEMW instruction – ensures memory transactions are visible */
#define XTENSA_MEMW()    __asm__ volatile ("memw" ::: "memory")

#endif /* NDF_COMPILER_H */
