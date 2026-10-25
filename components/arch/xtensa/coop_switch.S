/* void coop_switch(uint32_t *save, const uint32_t *load): CALL0 callee-saved context = a0 a1 a12 a13 a14 a15.
 * FPU / PIE state is caller-saved in the ABI (and lazily saved for ISRs), so it is not part of the context. */
    .text
    .align 4
    .global coop_switch
    .type coop_switch, @function
coop_switch:
    s32i    a0, a2, 0
    s32i    a1, a2, 4
    s32i    a12, a2, 8
    s32i    a13, a2, 12
    s32i    a14, a2, 16
    s32i    a15, a2, 20
    l32i    a0, a3, 0
    l32i    a1, a3, 4
    l32i    a12, a3, 8
    l32i    a13, a3, 12
    l32i    a14, a3, 16
    l32i    a15, a3, 20
    ret
    .size coop_switch, . - coop_switch
