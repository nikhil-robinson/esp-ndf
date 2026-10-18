#include "soc_caps.h"
/* Reset entry for both cores. The ROM loader jumps here (core0), and the
 * ROM's core1 boot stub jumps to the address in SYSTEM_CORE1_CTRL1 (core1).
 * We discard the ROM's windowed state and switch to CALL0: WOE = 0. */

#define PS_INIT  0x2F        /* UM = 1, EXCM = 0, WOE = 0, INTLEVEL = 15 */

    .section .iram.text, "ax"
    .literal_position

.macro CORE_START stack, cfn, vec
    movi    a0, PS_INIT
    wsr     a0, ps
    rsync
    movi    a0, 0
    wsr     a0, windowbase
    rsync
    movi    a0, 1
    wsr     a0, windowstart
    movi    a0, 0
    wsr     a0, intenable
    movi    a0, \vec
    wsr     a0, vecbase
    movi    a0, SOC_CPENABLE_BOOT   /* FPU (cp0) + PIE vector unit (cp3), per soc_caps.h */
    wsr     a0, cpenable
    rsync
    movi    a1, \stack
    movi    a0, 0
    call0   \cfn
1:  j       1b
.endm

    .global _start0
    .align 4
_start0:
    CORE_START _stack0_top, sys_start0, _vector_table0

    .global _start1
    .align 4
_start1:
    CORE_START _stack1_top, sys_start1, _vector_table1
