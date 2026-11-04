/* void coop_switch(uint32_t *save, const uint32_t *load): RISC-V ilp32f callee-saved context = ra sp s0-s11 fs0-fs11
 * (26 words, COOP_CTX_WORDS). Temporaries and the other FP registers are caller-saved in the ABI. */
#include "soc_caps.h"
    .text
    .align 2
    .global coop_switch
    .type coop_switch, @function
coop_switch:
    sw      ra, 0(a0)
    sw      sp, 4(a0)
    sw      s0, 8(a0)
    sw      s1, 12(a0)
    sw      s2, 16(a0)
    sw      s3, 20(a0)
    sw      s4, 24(a0)
    sw      s5, 28(a0)
    sw      s6, 32(a0)
    sw      s7, 36(a0)
    sw      s8, 40(a0)
    sw      s9, 44(a0)
    sw      s10, 48(a0)
    sw      s11, 52(a0)
#if SOC_CPU_HAS_FPU
    fsw     fs0, 56(a0)
    fsw     fs1, 60(a0)
    fsw     fs2, 64(a0)
    fsw     fs3, 68(a0)
    fsw     fs4, 72(a0)
    fsw     fs5, 76(a0)
    fsw     fs6, 80(a0)
    fsw     fs7, 84(a0)
    fsw     fs8, 88(a0)
    fsw     fs9, 92(a0)
    fsw     fs10, 96(a0)
    fsw     fs11, 100(a0)
    flw     fs0, 56(a1)
    flw     fs1, 60(a1)
    flw     fs2, 64(a1)
    flw     fs3, 68(a1)
    flw     fs4, 72(a1)
    flw     fs5, 76(a1)
    flw     fs6, 80(a1)
    flw     fs7, 84(a1)
    flw     fs8, 88(a1)
    flw     fs9, 92(a1)
    flw     fs10, 96(a1)
    flw     fs11, 100(a1)
#endif
    lw      ra, 0(a1)
    lw      sp, 4(a1)
    lw      s0, 8(a1)
    lw      s1, 12(a1)
    lw      s2, 16(a1)
    lw      s3, 20(a1)
    lw      s4, 24(a1)
    lw      s5, 28(a1)
    lw      s6, 32(a1)
    lw      s7, 36(a1)
    lw      s8, 40(a1)
    lw      s9, 44(a1)
    lw      s10, 48(a1)
    lw      s11, 52(a1)
    ret
    .size coop_switch, . - coop_switch
