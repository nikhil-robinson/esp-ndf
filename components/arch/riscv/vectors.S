/* RISC-V trap and CLIC interrupt entry (ESP32-P4).
 *
 * Every attached interrupt is CLIC-vectored (attr.shv = 1): the core loads its entry from mtvt[16 + n] and jumps
 * there with MIE cleared and the old level saved in mcause.mpil. Entries:
 *   - irq_entry:  saves only the caller-saved set (16 registers + mepc/mcause/mstatus = 80 bytes), turns the FPU off
 *                 (float use in a handler = illegal instruction fault, as on the S3), re-enables MIE so higher CLIC
 *                 levels nest (hardware nesting, IDF SOC_INT_HW_NESTED_SUPPORTED), calls irq_tab[core][n].fn(arg).
 *   - a naked handler (irq_naked): the table points straight at it; no frame at all, it ends with mret.
 * Exceptions (and any non-vectored interrupt) go to mtvec = _trap_vec: full register frame on the panic stack, then
 * ndf_fault(). Frame layouts are fixed: irq.c and sys.c index them. */
#include "soc_caps.h"

#define F_RA     0
#define F_MEPC   64
#define F_MCAUSE 68
#define F_MSTAT  72
#define F_SIZE   80

    .section .iram.text, "ax"
    .align 2
    .global irq_entry
    .type irq_entry, @function
irq_entry:
    addi    sp, sp, -F_SIZE
    sw      ra,  0(sp)
    sw      t0,  4(sp)
    sw      t1,  8(sp)
    sw      t2, 12(sp)
    sw      a0, 16(sp)
    sw      a1, 20(sp)
    sw      a2, 24(sp)
    sw      a3, 28(sp)
    sw      a4, 32(sp)
    sw      a5, 36(sp)
    sw      a6, 40(sp)
    sw      a7, 44(sp)
    sw      t3, 48(sp)
    sw      t4, 52(sp)
    sw      t5, 56(sp)
    sw      t6, 60(sp)
    csrr    t0, mcause
    csrr    t1, mepc
    csrr    t2, mstatus
    sw      t1, F_MEPC(sp)
    sw      t0, F_MCAUSE(sp)
    sw      t2, F_MSTAT(sp)
    li      t1, 0x6000
    csrc    mstatus, t1                 /* FS = Off inside handlers */
    andi    t0, t0, 0x3F                /* CLIC id = 16 + n */
    addi    t0, t0, -16
    slli    t0, t0, 3                   /* irq_slot_t = 8 bytes */
    csrr    t1, mhartid
    slli    t1, t1, 8                   /* 32 slots per core */
    add     t0, t0, t1
    la      t1, irq_tab
    add     t0, t0, t1
    lw      t1, 0(t0)
    lw      a0, 4(t0)
    csrsi   mstatus, 8                  /* nesting: only higher CLIC levels can enter now */
    jalr    t1
    csrci   mstatus, 8
    lw      t0, F_MEPC(sp)
    csrw    mepc, t0
    lw      t0, F_MCAUSE(sp)
    csrw    mcause, t0                  /* mret restores the interrupted level from mcause.mpil */
    lw      t0, F_MSTAT(sp)
    csrw    mstatus, t0                 /* MIE = 0, MPIE and FS as on entry */
    lw      ra,  0(sp)
    lw      t0,  4(sp)
    lw      t1,  8(sp)
    lw      t2, 12(sp)
    lw      a0, 16(sp)
    lw      a1, 20(sp)
    lw      a2, 24(sp)
    lw      a3, 28(sp)
    lw      a4, 32(sp)
    lw      a5, 36(sp)
    lw      a6, 40(sp)
    lw      a7, 44(sp)
    lw      t3, 48(sp)
    lw      t4, 52(sp)
    lw      t5, 56(sp)
    lw      t6, 60(sp)
    addi    sp, sp, F_SIZE
    mret
    .size irq_entry, . - irq_entry

/* Exception vector: CLIC mode needs mtvec 64-byte aligned. Frame (32 words): [0] = mepc, [n] = xn (n = 1..31,
 * [2] = sp at the trap). Runs on ndf_panic_stack[core]: start.S puts its top in mscratch. */
    .section .iram.text, "ax"
    .balign 64
    .global _trap_vec
_trap_vec:
    csrrw   sp, mscratch, sp        /* sp = this core's panic stack top (set at boot), mscratch = sp at the trap */
    addi    sp, sp, -128
    sw      x1,   4(sp)
    sw      x3,  12(sp)
    sw      x4,  16(sp)
    sw      x5,  20(sp)
    sw      x6,  24(sp)
    sw      x7,  28(sp)
    sw      x8,  32(sp)
    sw      x9,  36(sp)
    sw      x10, 40(sp)
    sw      x11, 44(sp)
    sw      x12, 48(sp)
    sw      x13, 52(sp)
    sw      x14, 56(sp)
    sw      x15, 60(sp)
    sw      x16, 64(sp)
    sw      x17, 68(sp)
    sw      x18, 72(sp)
    sw      x19, 76(sp)
    sw      x20, 80(sp)
    sw      x21, 84(sp)
    sw      x22, 88(sp)
    sw      x23, 92(sp)
    sw      x24, 96(sp)
    sw      x25, 100(sp)
    sw      x26, 104(sp)
    sw      x27, 108(sp)
    sw      x28, 112(sp)
    sw      x29, 116(sp)
    sw      x30, 120(sp)
    sw      x31, 124(sp)
    csrr    a5, mscratch
    sw      a5,   8(sp)             /* sp at the trap */
    csrr    a2, mepc
    sw      a2,   0(sp)
    csrr    a1, mcause
    srli    a0, a1, 31
    addi    a0, a0, 1               /* kind: 1 exception, 2 non-vectored interrupt */
    slli    a1, a1, 20
    srli    a1, a1, 20              /* cause code (12 bits) */
    csrr    a3, mtval
    mv      a4, sp                  /* frame */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    call    ndf_fault               /* does not return */
2:  j       2b
