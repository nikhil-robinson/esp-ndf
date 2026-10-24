/* ULP-RISC-V test program (linked at 0 = RTC slow memory 0x50000000): every timer start increments the word at 0x200,
 * (after clearing DONE / SHUT_RESET_EN as IDF's start.S) writes a magic at 0x204, copies the main CPU's word 0x208 + 1 to 0x20C (a two-way mailbox), then halts as IDF's
 * ulp_riscv_halt (COCPU_CTRL at 0x8104: SHUT_2_CLK_DIS = 0x3F, then DONE | SHUT_RESET_EN). */
    .section .text
    .org 0
    j       start
    .org 0x10
    j       start                     /* interrupt vector: unused */
start:
    li      sp, 0x400
    li      t0, 0x8104                /* as IDF's start: clear DONE | SHUT_RESET_EN, or the timer cannot restart us */
    lw      t1, 0(t0)
    li      t2, ~((1 << 25) | (1 << 22))
    and     t1, t1, t2
    sw      t1, 0(t0)
    lw      a0, 0x200(zero)
    addi    a0, a0, 1
    sw      a0, 0x200(zero)
    li      a1, 0x554C5021            /* "!PLU" */
    sw      a1, 0x204(zero)
    lw      a2, 0x208(zero)
    addi    a2, a2, 1
    sw      a2, 0x20C(zero)
    li      t0, 0x8104
    lw      t1, 0(t0)
    li      t2, 0x3F << 14
    or      t1, t1, t2
    sw      t1, 0(t0)
    li      t2, (1 << 25) | (1 << 22)
    or      t1, t1, t2
    sw      t1, 0(t0)
1:  j       1b
