/* RTC-boot probe stub, linked at 0x50000000 (RTC slow memory): the CPU's reset vector when
 * RTC_CNTL_PROCPU_STAT_VECTOR_SEL = 0 (TRM 10.6). First the vector goes back to the ROM (safety), then the system timer
 * and a marker into RTC memory, then a software system reset (the ROM boots the app, which reports). */
    .section .text, "ax"
    .global _stub
_stub:
    j       1f
    .align 4
lit_state:  .word 0x60008038          /* RTC_CNTL_RESET_STATE */
lit_bit13:  .word 0x00002000
lit_op:     .word 0x60023004          /* SYSTIMER_UNIT0_OP */
lit_lo:     .word 0x60023044
lit_mark:   .word 0x50001F00
lit_magic:  .word 0x5354424F          /* "STBO" */
lit_opt0:   .word 0x60008000          /* RTC_CNTL_OPTIONS0 */
lit_rst:    .word 0x80000000          /* SW_SYS_RST */
lit_upd:    .word 0x40000000
    .align 4
1:  l32r    a2, lit_state             /* vector back to the ROM first */
    l32r    a3, lit_bit13
    l32i    a4, a2, 0
    or      a4, a4, a3
    s32i    a4, a2, 0
    l32r    a2, lit_op                /* system timer value now */
    l32r    a3, lit_upd
    s32i    a3, a2, 0
    memw
2:  l32i    a4, a2, 0
    bbci    a4, 29, 2b
    l32r    a2, lit_lo
    l32i    a5, a2, 0
    l32r    a2, lit_mark
    s32i    a5, a2, 4
    l32r    a3, lit_magic
    s32i    a3, a2, 0
    memw
    l32r    a2, lit_opt0
    l32r    a3, lit_rst
    l32i    a4, a2, 0
    or      a4, a4, a3
    s32i    a4, a2, 0
3:  j       3b
