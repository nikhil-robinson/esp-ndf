/* Reset entry for both HP cores. The ROM jumps to _start0 (image entry) on core 0; core 1 starts at the address in
 * LP_SYSTEM_REG_BOOT_ADDR_HP_CORE1 (written by cpu1_start). Interrupts stay off (mstatus.MIE = 0) until irq_enable_all.
 * mtvec (CLIC mode 3) = _trap_vec for exceptions, mtvt = per-core table of vectored interrupt entries (vectors.S). */
#include "soc_caps.h"

    .section .iram.text, "ax"

.macro TRC c                           /* bring-up trace: one character on UART0 (NDF_TRACE_BOOT) */
#ifdef NDF_TRACE_BOOT
    li      t3, 0x500CA000
    li      t4, \c
    sw      t4, 0(t3)
#endif
.endm

.macro CORE_START stack, cfn, mtvt, core
    csrci   mstatus, 8                  /* MIE off */
    la      t0, ndf_panic_stack + (\core + 1) * 2048
    csrw    mscratch, t0                /* exception entry switches to it (vectors.S _trap_vec) */
    TRC '1'
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, \stack
    la      t0, _trap_vec
    ori     t0, t0, 3                   /* CLIC mode */
    csrw    mtvec, t0
    TRC '2'
    la      t0, \mtvt
    csrw    0x307, t0                   /* mtvt */
    TRC '3'
    li      t0, 0xFF
#if NDF_P4_REV >= 3
    csrw    0x347, t0                   /* mintthresh: everything masked until irq_enable_all */
#else
    slli    t0, t0, 24                  /* v1.x: memory-mapped CLIC threshold of this core (cpu.h) */
    li      t3, 0x20800008
    sw      t0, 0(t3)
#endif
    TRC '4'
#if SOC_CPU_HAS_FPU
    li      t0, 0x2000                  /* mstatus.FS = Initial: the C ABI (ilp32f) uses the FPU */
    csrs    mstatus, t0
    fscsr   zero
#endif
    TRC '5'
    csrw    mcycle, zero
    TRC '6'
    call    \cfn
1:  j       1b
.endm

    .global _start0
    .align 2
_start0:
#ifdef NDF_TRACE_BOOT
    li t0, 0x500CA000                   /* UART0 FIFO */
    li t1, 'S'
    sw t1, 0(t0)
#endif
#ifdef SOC_BOOT_ASM
    SOC_BOOT_ASM                        /* chip step before the first stack access (P4: L2 cache size, soc_caps.h) */
#endif
    CORE_START _stack0_top, sys_start0, irq_mtvt0, 0

    .global _start1
    .align 2
_start1:
    CORE_START _stack1_top, sys_start1, irq_mtvt1, 1
