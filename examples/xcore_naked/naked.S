/* Core 1, level 5, naked: cross-core interrupt CPU_INTR_FROM_CPU_1.
 * Clear the request, copy the mailbox (xcn[2] -> xcn[3]). Saves a2, a3. */
    .section .iram.text, "ax"
    .global naked_xc1
    .literal_position
    .align 4
naked_xc1:
    movi    a0, xcn
    s32i    a2, a0, 0
    s32i    a3, a0, 4
    movi    a2, 0x600C0034          /* SYSTEM_CPU_INTR_FROM_CPU_1 */
    movi    a3, 0
    s32i    a3, a2, 0               /* clear the request (level interrupt) */
    l32i    a3, a2, 0               /* read back: the clear must land before rfi, or the line re-fires */
    l32i    a3, a0, 16
    addi    a3, a3, 1
    s32i    a3, a0, 16              /* invocation count */
    l32i    a3, a0, 8
    s32i    a3, a0, 12              /* deliver */
    l32i    a3, a0, 4
    l32i    a2, a0, 0
    rsr     a0, excsave5
    rfi     5
