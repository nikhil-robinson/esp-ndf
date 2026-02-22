/* ===========================================================================
 * vectors.S  –  Xtensa LX7 Exception / Interrupt Vector Table for ESP32-S3
 *
 * This file defines all 17 fixed-offset vectors required by the Xtensa LX7
 * VECBASE mechanism.  VECBASE must be loaded into the special register SR
 * VECBASE before any interrupts are enabled; it is set in start.S.
 *
 * Memory layout (placed in .vectors section → start of IRAM):
 *
 *   VECBASE + 0x000  WindowOverflow4    (64-byte stub)
 *   VECBASE + 0x040  WindowUnderflow4
 *   VECBASE + 0x080  WindowOverflow8
 *   VECBASE + 0x0C0  WindowUnderflow8
 *   VECBASE + 0x100  WindowOverflow12
 *   VECBASE + 0x140  WindowUnderflow12
 *   VECBASE + 0x180  Level 2 IRQ
 *   VECBASE + 0x1C0  Level 3 IRQ
 *   VECBASE + 0x200  Level 4 IRQ
 *   VECBASE + 0x240  Level 5 IRQ
 *   VECBASE + 0x280  Level 6 Debug
 *   VECBASE + 0x2C0  Level 7 NMI
 *   VECBASE + 0x300  Kernel Exception
 *   VECBASE + 0x340  User Exception
 *   VECBASE + 0x3C0  Double Exception
 *
 * Each vector slot is exactly 0x40 = 64 bytes.  The Xtensa architecture
 * allows at most 12 instructions (3-byte or 4-byte each) in this space before
 * a JUMP to the real handler.
 *
 * See: Xtensa ISA Reference §4.4.1.5, ESP32-S3 TRM §CPU.
 * =========================================================================== */

#include "xtensa_ops.h"   /* XSR, RSR, WSR, CALL0 macros */

    .section .vectors, "ax"
    .global  _vectors_start
    .global  _vecbase

/* ---------------------------------------------------------------------------
 * Helper macro: jump to a C handler via CALL0 (no window change, safe from
 * vector context).  The actual handler must preserve all registers it uses.
 * --------------------------------------------------------------------------- */
.macro  VECTOR_CALL4  handler
    /* Save minimal state to the exception stack frame, then branch */
    call0   \handler
.endm

/* ===========================================================================
 * 0x000 – Window Overflow 4 (CALL4 window spill)
 *
 * Triggered automatically by the Xtensa windowed-register mechanism when a
 * CALL4 cannot rotate the register file.  The hardware requires this handler
 * to spill Aregisters a4-a7 of the overflowed window.
 * See: Xtensa ISA Ref §4.7.6.1
 * =========================================================================== */
    .org  0x000
    .global  _WindowOverflow4
_WindowOverflow4:
    s32e    a0, a5, -16     /* Save a0 of overflowed window to stack of caller */
    s32e    a1, a5, -12
    s32e    a2, a5, -8
    s32e    a3, a5, -4
    rfwo                    /* Return from Window Overflow */

    .org  0x040
/* ===========================================================================
 * 0x040 – Window Underflow 4 (CALL4 window refill)
 * =========================================================================== */
    .global  _WindowUnderflow4
_WindowUnderflow4:
    l32e    a0, a5, -16     /* Restore a0 from stack of caller */
    l32e    a1, a5, -12
    l32e    a2, a5, -8
    l32e    a3, a5, -4
    rfwu                    /* Return from Window Underflow */

    .org  0x080
/* ===========================================================================
 * 0x080 – Window Overflow 8 (CALL8 window spill)
 * =========================================================================== */
    .global  _WindowOverflow8
_WindowOverflow8:
    s32e    a0, a9, -16
    s32e    a1, a9, -12
    s32e    a2, a9, -8
    s32e    a3, a9, -4
    s32e    a4, a9, -32
    s32e    a5, a9, -28
    s32e    a6, a9, -24
    s32e    a7, a9, -20
    rfwo

    .org  0x0C0
/* ===========================================================================
 * 0x0C0 – Window Underflow 8 (CALL8 window refill)
 * =========================================================================== */
    .global  _WindowUnderflow8
_WindowUnderflow8:
    l32e    a0, a9, -16
    l32e    a1, a9, -12
    l32e    a2, a9, -8
    l32e    a3, a9, -4
    l32e    a4, a9, -32
    l32e    a5, a9, -28
    l32e    a6, a9, -24
    l32e    a7, a9, -20
    rfwu

    .org  0x100
/* ===========================================================================
 * 0x100 – Window Overflow 12 (CALL12 window spill)
 * =========================================================================== */
    .global  _WindowOverflow12
_WindowOverflow12:
    s32e    a0, a13, -16
    s32e    a1, a13, -12
    s32e    a2, a13, -8
    s32e    a3, a13, -4
    s32e    a4, a13, -48
    s32e    a5, a13, -44
    s32e    a6, a13, -40
    s32e    a7, a13, -36
    s32e    a8, a13, -32
    s32e    a9, a13, -28
    s32e    a10, a13, -24
    s32e    a11, a13, -20
    rfwo

    .org  0x140
/* ===========================================================================
 * 0x140 – Window Underflow 12 (CALL12 window refill)
 * =========================================================================== */
    .global  _WindowUnderflow12
_WindowUnderflow12:
    l32e    a0, a13, -16
    l32e    a1, a13, -12
    l32e    a2, a13, -8
    l32e    a3, a13, -4
    l32e    a4, a13, -48
    l32e    a5, a13, -44
    l32e    a6, a13, -40
    l32e    a7, a13, -36
    l32e    a8, a13, -32
    l32e    a9, a13, -28
    l32e    a10, a13, -24
    l32e    a11, a13, -20
    rfwu

/* ===========================================================================
 * 0x180 – Level 2 IRQ  (peripheral interrupt, e.g. software intr from CPU1)
 * =========================================================================== */
    .org  0x180
    .global  _Level2Vector
_Level2Vector:
    wsr     a0, EXCSAVE2
    call0   _xt_isr_handler_level2
    rsr     a0, EXCSAVE2
    rfi     2

/* ===========================================================================
 * 0x1C0 – Level 3 IRQ  (timer0 interrupt by default on ESP32-S3)
 * =========================================================================== */
    .org  0x1C0
    .global  _Level3Vector
_Level3Vector:
    wsr     a0, EXCSAVE3
    call0   _xt_isr_handler_level3
    rsr     a0, EXCSAVE3
    rfi     3

/* ===========================================================================
 * 0x200 – Level 4 IRQ
 * =========================================================================== */
    .org  0x200
    .global  _Level4Vector
_Level4Vector:
    wsr     a0, EXCSAVE4
    call0   _xt_isr_handler_level4
    rsr     a0, EXCSAVE4
    rfi     4

/* ===========================================================================
 * 0x240 – Level 5 IRQ  (profiling timer, etc.)
 * =========================================================================== */
    .org  0x240
    .global  _Level5Vector
_Level5Vector:
    wsr     a0, EXCSAVE5
    call0   _xt_isr_handler_level5
    rsr     a0, EXCSAVE5
    rfi     5

/* ===========================================================================
 * 0x280 – Level 6 Debug  (JTAG debug exception)
 * =========================================================================== */
    .org  0x280
    .global  _DebugVector
_DebugVector:
    wsr     a0, EXCSAVE6
    call0   _xt_debug_exception
    rsr     a0, EXCSAVE6
    rfi     6

/* ===========================================================================
 * 0x2C0 – Level 7 NMI  (non-maskable interrupt – watch-dog)
 * =========================================================================== */
    .org  0x2C0
    .global  _NMIVector
_NMIVector:
    wsr     a0, EXCSAVE7
    call0   _xt_nmi_handler
    rsr     a0, EXCSAVE7
    rfi     7

/* ===========================================================================
 * 0x300 – Kernel Exception  (UserExceptionMode=0, EXCM=1)
 * =========================================================================== */
    .org  0x300
    .global  _KernelExceptionVector
_KernelExceptionVector:
    wsr     a0, EXCSAVE1
    call0   _xt_kernel_exception
    /* Not reached unless handler returns (it shouldn't) */
1:  j       1b

/* ===========================================================================
 * 0x340 – User Exception  (UserExceptionMode=1, covers syscall, memory faults)
 * =========================================================================== */
    .org  0x340
    .global  _UserExceptionVector
_UserExceptionVector:
    wsr     a0, EXCSAVE1
    call0   _xt_user_exception
1:  j       1b

/* ===========================================================================
 * 0x3C0 – Double Exception  (exception taken while EXCM=1; usually fatal)
 * =========================================================================== */
    .org  0x3C0
    .global  _DoubleExceptionVector
_DoubleExceptionVector:
    /* There is no safe way to recover; spin in place so JTAG can attach */
1:  j       1b

    /* Pad to 0x400 so VECBASE alignment invariant is satisfied */
    .org  0x400

/* ---------------------------------------------------------------------------
 * Default ISR stubs (weak aliases – override by defining the symbol in C)
 *
 * These default handlers simply return without acknowledgement so the CPU
 * does not lock up if an unexpected interrupt fires during development.
 * In production code every active interrupt source must have a real handler.
 * --------------------------------------------------------------------------- */
    .section .iram.text, "ax"   /* MUST be in IRAM – within CALL0 range of vectors */

.macro  DEFAULT_ISR  name, level
    .global  \name
    .weak    \name
    .align 4
\name:
    /* Acknowledge the interrupt at the intctrl level, then return */
    rfi     \level
.endm

DEFAULT_ISR _xt_isr_handler_level2, 2
DEFAULT_ISR _xt_isr_handler_level3, 3
DEFAULT_ISR _xt_isr_handler_level4, 4
DEFAULT_ISR _xt_isr_handler_level5, 5

    .global  _xt_debug_exception
    .weak    _xt_debug_exception
    .align 4
_xt_debug_exception:
    rfi     6

    .global  _xt_nmi_handler
    .weak    _xt_nmi_handler
    .align 4
_xt_nmi_handler:
    rfi     7

    .global  _xt_kernel_exception
    .weak    _xt_kernel_exception
    .align 4
_xt_kernel_exception:
1:  j       1b

    .global  _xt_user_exception
    .weak    _xt_user_exception
    .align 4
_xt_user_exception:
1:  j       1b
