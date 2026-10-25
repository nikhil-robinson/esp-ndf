/* Level-5 naked handler for CCOMPARE2 (CPU int 16): record CCOUNT on entry
 * and exit, clear the timer. Uses a0 (free) + a2 (saved to memory). */
    .section .iram.text, "ax"
    .global naked_cc2
    .literal_position
    .align 4
naked_cc2:
    movi    a0, naked_probe
    s32i    a2, a0, 0           /* save a2 */
    rsr     a2, ccount
    s32i    a2, a0, 4           /* t_in */
    addi    a2, a2, -16
    wsr     a2, ccompare2       /* clears the interrupt; match set safely in the past */
    addi    a2, a2, 16
    movi    a2, 1
    s32i    a2, a0, 8           /* done */
    rsr     a2, ccount
    s32i    a2, a0, 12          /* t_out */
    l32i    a2, a0, 0
    rsr     a0, excsave5
    rfi     5
