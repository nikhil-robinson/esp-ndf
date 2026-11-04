/* Xtensa LX7 special-register helpers. CALL0 ABI throughout (no windows). */
#pragma once
#include "ndf.h"

ALWAYS_INLINE uint32_t ccount(void)
{
    uint32_t c;
    __asm__ volatile("rsr.ccount %0" : "=r"(c));
    return c;
}

ALWAYS_INLINE int core_id(void)
{
    uint32_t p;
    __asm__ volatile("rsr.prid %0" : "=r"(p));
    return (p >> 13) & 1;
}

/* Mask interrupts up to level 5 (keeps debug/NMI); returns the old PS. */
ALWAYS_INLINE uint32_t irq_lock(void)
{
    uint32_t ps;
    __asm__ volatile("rsil %0, 5" : "=r"(ps) :: "memory");
    return ps;
}

ALWAYS_INLINE void irq_unlock(uint32_t ps)
{
    __asm__ volatile("wsr.ps %0; rsync" :: "r"(ps) : "memory");
}

ALWAYS_INLINE void irq_enable_all(void)
{
    uint32_t ps;
    __asm__ volatile("rsil %0, 0" : "=r"(ps) :: "memory");
}

/* sleep until an interrupt (waiti 0), shared code through cpu.h */
ALWAYS_INLINE void cpu_wait_for_irq(void) { __asm__ volatile("waiti 0"); }
