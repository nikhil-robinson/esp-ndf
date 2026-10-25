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
 * FPU in handlers, lazily: handlers start with CPENABLE = 0. The first FP
 * instruction traps (Coprocessor0Disabled); the trap saves the FP registers
 * (and BR) of the interrupted context into fpu_save[core][INTLEVEL] (nested
 * handlers always have a higher INTLEVEL, so the level names the owner) and
 * returns with CP0 on. Handler exit restores them when CP0 is on: 2
 * instructions on the common no-FP path (+2 on level 1 for EPC1, which the
 * trap overwrites). */

#include "soc_intr.h"

#define PS_UM       0x20
#define PS_EXCM     0x10
#define FRAME       80     /* 16 saved words + PS (level 1) + pad, 16-aligned */
#define FPU_SLOT    76     /* f0-f15, FCR, FSR, BR */
#define PIE_SLOT    208    /* IDF XCHAL_CP3_SA_SIZE */

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
    .org _vector_table\c + 0x280   /* level 6 = debug (watchpoints) */
    wsr a0, excsave6
    j   _l6_entry
    .org _vector_table\c + 0x2C0   /* level 7 = NMI (irq_attach_nmi) */
    wsr a0, excsave7
    j   _l7_entry
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

/* Handler at INTLEVEL lvl used the FPU (CP0) and/or PIE (CP3): they are still on (it trapped, since every
 * handler starts with CPENABLE = 0): put back what it displaced. 2 instructions when neither was used. */
.macro FPU_EXIT lvl
    rsr     a5, cpenable
    beqz    a5, .Lfx\@
    rsr     a2, prid
    extui   a2, a2, 13, 1
    bbci    a5, 0, .Lfp\@
    movi    a3, FPU_SLOT * 6
    mull    a3, a3, a2
    movi    a4, fpu_save + FPU_SLOT * \lvl
    add     a3, a3, a4
    FPU_LOAD a3
.Lfp\@:
    bbci    a5, 3, .Lfx\@
    movi    a3, PIE_SLOT * 6
    mull    a3, a3, a2
    movi    a4, pie_save + PIE_SLOT * \lvl
    add     a3, a3, a4
    PIE_LOAD a3
.Lfx\@:
.endm

/* PIE (cop_ai) state, IDF xchal_cp3_store/load layout: 208 bytes, 16-aligned. a4 scratch, r advanced. */
.macro PIE_STORE r
    rur.accx_0 a4
    s32i a4, \r, 0
    rur.accx_1 a4
    s32i a4, \r, 4
    rur.qacc_h_0 a4
    s32i a4, \r, 8
    rur.qacc_h_1 a4
    s32i a4, \r, 12
    rur.qacc_h_2 a4
    s32i a4, \r, 16
    rur.qacc_h_3 a4
    s32i a4, \r, 20
    rur.qacc_h_4 a4
    s32i a4, \r, 24
    rur.qacc_l_0 a4
    s32i a4, \r, 28
    rur.qacc_l_1 a4
    s32i a4, \r, 32
    rur.qacc_l_2 a4
    s32i a4, \r, 36
    rur.qacc_l_3 a4
    s32i a4, \r, 40
    rur.qacc_l_4 a4
    s32i a4, \r, 44
    rur.sar_byte a4
    s32i a4, \r, 48
    rur.fft_bit_width a4
    s32i a4, \r, 52
    rur.ua_state_0 a4
    s32i a4, \r, 56
    rur.ua_state_1 a4
    s32i a4, \r, 60
    rur.ua_state_2 a4
    s32i a4, \r, 64
    rur.ua_state_3 a4
    s32i a4, \r, 68
    st.qr q0, \r, 80
    st.qr q1, \r, 96
    st.qr q2, \r, 112
    addi \r, \r, 128
    st.qr q3, \r, 0
    st.qr q4, \r, 16
    st.qr q5, \r, 32
    st.qr q6, \r, 48
    st.qr q7, \r, 64
.endm

.macro PIE_LOAD r
    l32i a4, \r, 0
    wur.accx_0 a4
    l32i a4, \r, 4
    wur.accx_1 a4
    l32i a4, \r, 8
    wur.qacc_h_0 a4
    l32i a4, \r, 12
    wur.qacc_h_1 a4
    l32i a4, \r, 16
    wur.qacc_h_2 a4
    l32i a4, \r, 20
    wur.qacc_h_3 a4
    l32i a4, \r, 24
    wur.qacc_h_4 a4
    l32i a4, \r, 28
    wur.qacc_l_0 a4
    l32i a4, \r, 32
    wur.qacc_l_1 a4
    l32i a4, \r, 36
    wur.qacc_l_2 a4
    l32i a4, \r, 40
    wur.qacc_l_3 a4
    l32i a4, \r, 44
    wur.qacc_l_4 a4
    l32i a4, \r, 48
    wur.sar_byte a4
    l32i a4, \r, 52
    wur.fft_bit_width a4
    l32i a4, \r, 56
    wur.ua_state_0 a4
    l32i a4, \r, 60
    wur.ua_state_1 a4
    l32i a4, \r, 64
    wur.ua_state_2 a4
    l32i a4, \r, 68
    wur.ua_state_3 a4
    ld.qr q0, \r, 80
    ld.qr q1, \r, 96
    ld.qr q2, \r, 112
    addi \r, \r, 128
    ld.qr q3, \r, 0
    ld.qr q4, \r, 16
    ld.qr q5, \r, 32
    ld.qr q6, \r, 48
    ld.qr q7, \r, 64
.endm

.macro FPU_LOAD r
    lsi f0, \r, 0
    lsi f1, \r, 4
    lsi f2, \r, 8
    lsi f3, \r, 12
    lsi f4, \r, 16
    lsi f5, \r, 20
    lsi f6, \r, 24
    lsi f7, \r, 28
    lsi f8, \r, 32
    lsi f9, \r, 36
    lsi f10, \r, 40
    lsi f11, \r, 44
    lsi f12, \r, 48
    lsi f13, \r, 52
    lsi f14, \r, 56
    lsi f15, \r, 60
    l32i a4, \r, 64
    wur.fcr a4
    l32i a4, \r, 68
    wur.fsr a4
    l32i a4, \r, 72
    wsr  a4, br
.endm

.macro FPU_STORE r
    ssi f0, \r, 0
    ssi f1, \r, 4
    ssi f2, \r, 8
    ssi f3, \r, 12
    ssi f4, \r, 16
    ssi f5, \r, 20
    ssi f6, \r, 24
    ssi f7, \r, 28
    ssi f8, \r, 32
    ssi f9, \r, 36
    ssi f10, \r, 40
    ssi f11, \r, 44
    ssi f12, \r, 48
    ssi f13, \r, 52
    ssi f14, \r, 56
    ssi f15, \r, 60
    rur.fcr a4
    s32i a4, \r, 64
    rur.fsr a4
    s32i a4, \r, 68
    rsr  a4, br                      /* FP compares write booleans: BR belongs to the FP state here */
    s32i a4, \r, 72
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
    rsr     a2, epc1                 /* the lazy-FPU trap in a handler overwrites EPC1 */
    s32i    a2, a1, 72
    movi    a2, PS_UM | 1            /* EXCM=0, INTLEVEL=1: allow nesting */
    wsr     a2, ps
    rsync
    DISPATCH SOC_INTR_L1_MASK
    FPU_EXIT 1
    l32i    a2, a1, 72
    wsr     a2, epc1
    l32i    a2, a1, 64               /* back to EXCM=1 + original INTLEVEL for rfe */
    wsr     a2, ps
    rsync
    RESTORE
    rfe

_l1_exception:
    beqi    a2, 32, 1f               /* 32 = Coprocessor0Disabled (FPU) */
    movi    a3, 35                   /* 35 = Coprocessor3Disabled (PIE) */
    bne     a2, a3, _l1_fault
1:  rsr     a3, ps
    extui   a3, a3, 0, 4             /* INTLEVEL: the handler that touched the coprocessor */
    beqz    a3, _l1_fault            /* not in a handler: it was off on purpose */
    bgei    a3, 6, _l1_fault
    rsr     a6, prid
    extui   a6, a6, 13, 1            /* core */
    movi    a7, 6
    mull    a7, a7, a6
    add     a7, a7, a3               /* slot index core * 6 + lvl */
    l32i    a4, a1, 60               /* the handler's CPENABLE (saved by this trap's SAVE) */
    bnei    a2, 32, 2f
    addi    a4, a4, 1                /* + CP0 */
    s32i    a4, a1, 60               /* the handler resumes with it on */
    wsr     a4, cpenable
    rsync
    movi    a5, FPU_SLOT
    mull    a5, a5, a7
    movi    a4, fpu_save
    add     a5, a5, a4
    FPU_STORE a5
    j       3f
2:  addi    a4, a4, 8                /* + CP3 */
    s32i    a4, a1, 60
    wsr     a4, cpenable
    rsync
    movi    a5, PIE_SLOT
    mull    a5, a5, a7
    movi    a4, pie_save
    add     a5, a5, a4
    PIE_STORE a5
3:  RESTORE
    rfe                              /* re-runs the instruction */

_l1_fault:                           /* frame holds a0..a11; add a12..a15 for the dump */
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
    rsr     a8, prid                 /* the report runs on this core's own panic stack: a smashed or */
    extui   a8, a8, 13, 1            /* overflowed task stack cannot take the handler down with it */
    movi    a9, ndf_panic_stack
    slli    a8, a8, 11               /* core * 2048 */
    add     a9, a9, a8
    movi    a10, 2048 - 16
    add     a1, a9, a10
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
    FPU_EXIT \lvl
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
    FPU_EXIT \lvl
    RESTORE
    rfi     \lvl
.endm

/* level 6, debug exception without a debugger: data watchpoints (debug.c) */
    .align 4
_l6_entry:
    SAVE    excsave6, PS_UM | 6
    rsr     a2, debugcause
    rsr     a3, epc6
    mov     a4, a1
    call0   ndf_debug_exc
    RESTORE
    rfi     6

/* level 7, NMI: not maskable (runs inside irq_lock); one handler per core, no FPU/PIE in it */
    .align 4
_l7_entry:
    SAVE    excsave7, PS_UM | 7
    rsr     a2, prid
    extui   a2, a2, 13, 1
    movi    a3, irq_nmi
    addx8   a3, a2, a3               /* irq_nmi[core] = { fn, arg } */
    l32i    a4, a3, 0
    l32i    a2, a3, 4
    beqz    a4, 1f
    callx0  a4
1:  RESTORE
    rfi     7

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

    .section .bss
    .align 4
    .global fpu_save, pie_save
fpu_save:   .space FPU_SLOT * 6 * 2  /* [core][INTLEVEL 0..5] */
    .align 16
pie_save:   .space PIE_SLOT * 6 * 2
