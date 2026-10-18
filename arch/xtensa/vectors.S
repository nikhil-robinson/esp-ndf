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
 * Levels 2..5 arrive with INTLEVEL = n and EXCM = 1; we clear EXCM (keeping
 * INTLEVEL = n), because with EXCM set the core never loops back in a
 * zero-overhead LOOP: every C loop in a handler would run its body once.
 *
 * FPU is disabled (CPENABLE = 0) inside handlers: an ISR touching floats
 * traps into ndf_fault() instead of corrupting the interrupted FP state. */

#include "soc_intr.h"

#define PS_UM       0x20
#define PS_EXCM     0x10
#define FRAME       80     /* 16 saved words + PS (level 1) + pad, 16-aligned */

/* ---- vector tables (VECBASE, 1 KB aligned), one per core --------------
 * Same content; separate copies so a naked level-4/5 handler can be patched
 * in as a direct jump for one core (irq_naked()). */
.macro VECTOR_TABLE c
    .align  1024
_vector_table\c:
    .org _vector_table\c + 0x000   /* window over/underflow: impossible with WOE=0 */
    j   _unexpected
    .org _vector_table\c + 0x040
    j   _unexpected
    .org _vector_table\c + 0x080
    j   _unexpected
    .org _vector_table\c + 0x0C0
    j   _unexpected
    .org _vector_table\c + 0x100
    j   _unexpected
    .org _vector_table\c + 0x140
    j   _unexpected
    .org _vector_table\c + 0x180   /* level 2 */
    wsr a0, excsave2
    j   _l2_entry
    .org _vector_table\c + 0x1C0   /* level 3 */
    wsr a0, excsave3
    j   _l3_entry
    .org _vector_table\c + 0x200   /* level 4 */
    wsr a0, excsave4
    .global _vec_l4_jump\c
_vec_l4_jump\c:
    j   _l4_entry                   /* patched by irq_naked() */
    .org _vector_table\c + 0x240   /* level 5 */
    wsr a0, excsave5
    .global _vec_l5_jump\c
_vec_l5_jump\c:
    j   _l5_entry
    .org _vector_table\c + 0x280   /* level 6 = debug */
    j   _unexpected
    .org _vector_table\c + 0x2C0   /* level 7 = NMI */
    j   _unexpected
    .org _vector_table\c + 0x300   /* kernel exception: impossible, PS.UM = 1 */
    j   _unexpected
    .org _vector_table\c + 0x340   /* user exception + level-1 interrupt */
    wsr a0, excsave1
    j   _l1_entry
    .org _vector_table\c + 0x3C0   /* double exception: fault inside EXCM window */
    j   _double
    .org _vector_table\c + 0x400
.endm

    .section .vectors, "ax"
    .global _vector_table, _vector_table0, _vector_table1
_vector_table:
    VECTOR_TABLE 0
    VECTOR_TABLE 1

/* ---- shared save / restore ---------------------------------------------- */
    .section .iram.text, "ax"
    .literal_position

.macro SAVE excsave, ps=0
    addi    a1, a1, -FRAME
    s32i    a2, a1, 4
    rsr     a2, \excsave
    s32i    a2, a1, 0           /* original a0 */
    s32i    a3, a1, 8
.if \ps
    movi    a3, \ps             /* new PS early: the 15 instructions below cover */
    wsr     a3, ps              /* the pipeline delay, so no rsync is needed */
.endif
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

_l1_exception:                       /* frame holds a0..a11; add a12..a15 for the dump */
    s32i    a12, a1, 64
    s32i    a13, a1, 68
    s32i    a14, a1, 72
    s32i    a15, a1, 76
    movi    a2, 1                   /* kind 1 = synchronous exception */
    rsr     a3, exccause
    rsr     a4, epc1
    rsr     a5, excvaddr
    mov     a6, a1                   /* saved registers */
    addi    a7, a1, FRAME            /* stack pointer of the faulting code */
    movi    a8, PS_UM | 5            /* EXCM = 0 so C loops work in the report; levels <= 5 masked */
    wsr     a8, ps
    rsync
    call0   ndf_fault

/* ---- levels 2..5 -------------------------------------------------------- */
.macro HL_ENTRY lvl, mask
    .align 4
_l\lvl\()_entry:
    SAVE    excsave\lvl, PS_UM | \lvl   /* EXCM = 0: zero-overhead loops only loop back with it clear; rfi restores PS from EPSn */
    DISPATCH \mask
    RESTORE
    rfi     \lvl
.endm

/* fast: no search. Handler must clear its own source (as on Cortex-M). */
.macro FAST_ENTRY lvl, slot
    .align 4
_l\lvl\()_entry:
    SAVE    excsave\lvl, PS_UM | \lvl   /* EXCM = 0: zero-overhead loops only loop back with it clear; rfi restores PS from EPSn */
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

/* Naked level 4/5, for handlers out of J range (+-128 KB) of the vector:
 * core c's vector jumps to _lN_naked_c, which jumps to irq_naked_fn[N-4][c].
 * Only a0 is free (original in EXCSAVEn); the handler saves what it uses
 * and returns with "rsr a0, excsaveN; rfi N". */
.macro NAKED_REDIRECT lvl, slot, c
    .align 4
    .global _l\lvl\()_naked_\c
_l\lvl\()_naked_\c:
    movi    a0, irq_naked_fn + \slot * 8 + \c * 4
    l32i    a0, a0, 0
    jx      a0
.endm
    NAKED_REDIRECT 4, 0, 0
    NAKED_REDIRECT 4, 0, 1
    NAKED_REDIRECT 5, 1, 0
    NAKED_REDIRECT 5, 1, 1

/* ---- faults ------------------------------------------------------------- */
    .align 4
_double:
    movi    a2, 2                    /* kind 2 = double exception */
    rsr     a3, exccause
    rsr     a4, depc
    rsr     a5, excvaddr
    movi    a6, 0                    /* no saved frame */
    mov     a7, a1
    movi    a8, PS_UM | 5            /* EXCM = 0 so C loops work in the report; levels <= 5 masked */
    wsr     a8, ps
    rsync
    call0   ndf_fault

    .align 4
_unexpected:
    movi    a2, 3                    /* kind 3 = unexpected vector */
    rsr     a3, exccause
    rsr     a4, epc1
    rsr     a5, excvaddr
    movi    a6, 0
    mov     a7, a1
    movi    a8, PS_UM | 5            /* EXCM = 0 so C loops work in the report; levels <= 5 masked */
    wsr     a8, ps
    rsync
    call0   ndf_fault
