/* Xtensa LX7 vector table + interrupt entry, CALL0 ABI (PS.WOE = 0).
 *
 * Interrupt cost model: save the 11 caller-saved regs + SAR + loop regs +
 * CPENABLE (16 words), then
 *   levels 1-3: inline dispatch loop over pending ints of that level,
 *   levels 4-5: "fast" - one handler per level per core, called directly.
 * Handler table layout (irq.c): irq_tab[core][32] {fn, arg};
 * irq_fast[core][2] {fn, arg} for levels 4, 5. No windows anywhere.
 *
 * Level 1 arrives with PS.EXCM = 1, which also masks levels 2-3. We drop to
 * PS.INTLEVEL = 1 / EXCM = 0 so levels 2..5 preempt level-1 handlers
 * (NVIC-style nesting) and exceptions inside handlers report as faults.
 * Levels 2..5 arrive with INTLEVEL = n set by hardware; nothing to change.
 *
 * FPU is disabled (CPENABLE = 0) inside handlers: an ISR touching floats
 * traps into ndf_fault() instead of corrupting the interrupted FP state. */

#include "soc_intr.h"

#define PS_UM       0x20
#define PS_EXCM     0x10
#define FRAME       80     /* 16 saved words + PS (level 1) + pad, 16-aligned */

/* ---- vector table (VECBASE, 1 KB aligned) ------------------------------- */
    .section .vectors, "ax"
    .global _vector_table
    .align  1024
_vector_table:
    .org 0x000                  /* window over/underflow: impossible with WOE=0 */
    j   _unexpected
    .org 0x040
    j   _unexpected
    .org 0x080
    j   _unexpected
    .org 0x0C0
    j   _unexpected
    .org 0x100
    j   _unexpected
    .org 0x140
    j   _unexpected
    .org 0x180                  /* level 2 */
    wsr a0, excsave2
    j   _l2_entry
    .org 0x1C0                  /* level 3 */
    wsr a0, excsave3
    j   _l3_entry
    .org 0x200                  /* level 4 */
    wsr a0, excsave4
    j   _l4_entry
    .org 0x240                  /* level 5 */
    wsr a0, excsave5
    j   _l5_entry
    .org 0x280                  /* level 6 = debug */
    j   _unexpected
    .org 0x2C0                  /* level 7 = NMI */
    j   _unexpected
    .org 0x300                  /* kernel exception: impossible, PS.UM = 1 */
    j   _unexpected
    .org 0x340                  /* user exception + level-1 interrupt */
    wsr a0, excsave1
    j   _l1_entry
    .org 0x3C0                  /* double exception: fault inside EXCM window */
    j   _double
    .org 0x400

/* ---- shared save / restore ---------------------------------------------- */
    .section .iram.text, "ax"
    .literal_position

.macro SAVE excsave
    addi    a1, a1, -FRAME
    s32i    a2, a1, 4
    rsr     a2, \excsave
    s32i    a2, a1, 0           /* original a0 */
    s32i    a3, a1, 8
    s32i    a4, a1, 12
    s32i    a5, a1, 16
    s32i    a6, a1, 20
    s32i    a7, a1, 24
    s32i    a8, a1, 28
    s32i    a9, a1, 32
    s32i    a10, a1, 36
    s32i    a11, a1, 40
    rsr     a2, sar
    s32i    a2, a1, 44
    rsr     a2, lbeg
    s32i    a2, a1, 48
    rsr     a2, lend
    s32i    a2, a1, 52
    rsr     a2, lcount
    s32i    a2, a1, 56
    rsr     a2, cpenable
    s32i    a2, a1, 60
    movi    a2, 0
    wsr     a2, cpenable
.endm

.macro RESTORE
    l32i    a2, a1, 60
    wsr     a2, cpenable
    l32i    a2, a1, 56
    wsr     a2, lcount
    l32i    a2, a1, 52
    wsr     a2, lend
    l32i    a2, a1, 48
    wsr     a2, lbeg
    l32i    a2, a1, 44
    wsr     a2, sar
    rsync
    l32i    a0, a1, 0
    l32i    a2, a1, 4
    l32i    a3, a1, 8
    l32i    a4, a1, 12
    l32i    a5, a1, 16
    l32i    a6, a1, 20
    l32i    a7, a1, 24
    l32i    a8, a1, 28
    l32i    a9, a1, 32
    l32i    a10, a1, 36
    l32i    a11, a1, 40
    addi    a1, a1, FRAME
.endm

/* Call every pending + enabled handler of this level, lowest int first,
 * until none remain. a2-a11 are free (saved); handlers clobber them, so the
 * table base lives in the frame (slot 68). */
.macro DISPATCH mask
    rsr     a2, prid
    movi    a3, irq_tab
    extui   a2, a2, 13, 1
    slli    a2, a2, 8                /* core * 32 slots * 8 bytes */
    add     a3, a3, a2
    s32i    a3, a1, 68
.Ld\@:
    rsr     a2, interrupt
    rsr     a4, intenable
    movi    a5, \mask
    and     a2, a2, a4
    and     a2, a2, a5
    beqz    a2, .Lx\@
    neg     a4, a2
    and     a4, a4, a2               /* lowest pending bit */
    wsr     a4, intclear             /* edge / software ints; no-op for level */
    nsau    a4, a4
    movi    a5, 31
    sub     a4, a5, a4               /* bit number */
    l32i    a3, a1, 68
    addx8   a3, a4, a3
    l32i    a5, a3, 0
    l32i    a2, a3, 4
    callx0  a5
    j       .Ld\@
.Lx\@:
.endm

/* ---- level 1 (and synchronous exceptions) ------------------------------- */
    .align 4
_l1_entry:
    SAVE    excsave1
    rsr     a2, exccause
    bnei    a2, 4, _l1_exception     /* 4 = Level1InterruptCause */
    rsr     a2, ps                   /* interrupted PS (+EXCM): restored exactly */
    s32i    a2, a1, 64
    movi    a2, PS_UM | 1            /* EXCM=0, INTLEVEL=1: allow nesting */
    wsr     a2, ps
    rsync
    DISPATCH SOC_INTR_L1_MASK
    l32i    a2, a1, 64               /* back to EXCM=1 + original INTLEVEL for rfe */
    wsr     a2, ps
    rsync
    RESTORE
    rfe

_l1_exception:                       /* frame holds the regs */
    movi    a2, 1                   /* kind 1 = synchronous exception */
    rsr     a3, exccause
    rsr     a4, epc1
    rsr     a5, excvaddr
    call0   ndf_fault                /* never returns */

/* ---- levels 2..5 -------------------------------------------------------- */
.macro HL_ENTRY lvl, mask
    .align 4
_l\lvl\()_entry:
    SAVE    excsave\lvl
    DISPATCH \mask
    RESTORE
    rfi     \lvl
.endm

/* fast: no search. Handler must clear its own source (as on Cortex-M). */
.macro FAST_ENTRY lvl, slot
    .align 4
_l\lvl\()_entry:
    SAVE    excsave\lvl
    rsr     a2, prid
    movi    a3, irq_fast + \slot * 8
    extui   a2, a2, 13, 1
    addx8   a3, a2, a3               /* + core * 16 */
    addx8   a3, a2, a3
    l32i    a4, a3, 0
    l32i    a2, a3, 4
    callx0  a4
    RESTORE
    rfi     \lvl
.endm

    HL_ENTRY 2, SOC_INTR_L2_MASK
    HL_ENTRY 3, SOC_INTR_L3_MASK
    FAST_ENTRY 4, 0
    FAST_ENTRY 5, 1

/* ---- faults ------------------------------------------------------------- */
    .align 4
_double:
    movi    a2, 2                    /* kind 2 = double exception */
    rsr     a3, exccause
    rsr     a4, depc
    rsr     a5, excvaddr
    call0   ndf_fault

    .align 4
_unexpected:
    movi    a2, 3                    /* kind 3 = unexpected vector */
    rsr     a3, exccause
    rsr     a4, epc1
    rsr     a5, excvaddr
    call0   ndf_fault
