/* RISC-V (ESP32-P4 HP core) CPU helpers, same API as arch/xtensa/xtensa.h.
 * Interrupt masking uses the CLIC threshold (mintthresh), not mstatus.MIE: irq_lock() masks levels 1-5 and leaves
 * levels 6-7 open (the NMI-equivalent path irq_attach_nmi uses), exactly like Xtensa "rsil 5". */
#pragma once
#include "ndf.h"

#define CSR_MINTTHRESH 0x347          /* standard CLIC threshold CSR (P4 v3; IDF INTTHRESH_STANDARD) */
#define CLIC_NLBITS    3
#define CLIC_LVL_BYTE(l) ((uint32_t)(l) << (8 - CLIC_NLBITS) | ((1u << (8 - CLIC_NLBITS)) - 1))   /* IDF NLBITS_TO_BYTE */
#define IRQ_LOCK_THRESH CLIC_LVL_BYTE(5)

ALWAYS_INLINE uint32_t ccount(void)
{
    uint32_t c;
    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    return c;
}

ALWAYS_INLINE int core_id(void)
{
    uint32_t h;
    __asm__ volatile("csrr %0, mhartid" : "=r"(h));
    return (int)h;
}

/* Mask interrupt levels 1-5; returns the old threshold. The CSR write takes effect for the next instruction. */
ALWAYS_INLINE uint32_t irq_lock(void)
{
    uint32_t old;
    __asm__ volatile("csrrw %0, 0x347, %1" : "=r"(old) : "r"(IRQ_LOCK_THRESH) : "memory");
    return old;
}

ALWAYS_INLINE void irq_unlock(uint32_t old)
{
    __asm__ volatile("csrw 0x347, %0" :: "r"(old) : "memory");
}

ALWAYS_INLINE void irq_enable_all(void)
{
    __asm__ volatile("csrw 0x347, zero\n\tcsrsi mstatus, 8" ::: "memory");
}

/* ordering between the cores / with DMA (Xtensa memw) */
ALWAYS_INLINE void cpu_barrier(void) { __asm__ volatile("fence" ::: "memory"); }

/* sleep until an interrupt */
ALWAYS_INLINE void cpu_wait_for_irq(void) { __asm__ volatile("wfi"); }

#define COOP_CTX_WORDS 26         /* coop_switch context: ra sp s0-s11 fs0-fs11 */
