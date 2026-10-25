/* Single-precision divide and square root on the LX7 FPU helper
 * instructions (div0/sqrt0 seed, maddn Newton steps, divn final rounding):
 * correctly rounded IEEE results, including denormals and specials.
 * Same instruction sequence as the Cadence/ESP ROM routines, CALL0 ABI
 * (no windowed entry/retw). Usable in interrupt handlers too (lazy FPU save, components/arch/xtensa/vectors.S). */
#include "soc_caps.h"
#if SOC_CPU_HAS_FPU
    .text
    .literal_position

    .global __divsf3
    .type   __divsf3, @function
    .align  4
__divsf3:
    wfr       f1, a2
    wfr       f2, a3
    div0.s    f3, f2
    nexp01.s  f4, f2
    const.s   f5, 1
    maddn.s   f5, f4, f3
    mov.s     f6, f3
    mov.s     f7, f2
    nexp01.s  f2, f1
    maddn.s   f6, f5, f6
    const.s   f5, 1
    const.s   f0, 0
    neg.s     f8, f2
    maddn.s   f5, f4, f6
    maddn.s   f0, f8, f3
    mkdadj.s  f7, f1
    maddn.s   f6, f5, f6
    maddn.s   f8, f4, f0
    const.s   f3, 1
    maddn.s   f3, f4, f6
    maddn.s   f0, f8, f6
    neg.s     f2, f2
    maddn.s   f6, f3, f6
    maddn.s   f2, f4, f0
    addexpm.s f0, f7
    addexp.s  f6, f7
    divn.s    f0, f2, f6
    rfr       a2, f0
    ret
    .size   __divsf3, . - __divsf3

    .global sqrtf
    .type   sqrtf, @function
    .align  4
sqrtf:
    wfr       f1, a2
    sqrt0.s   f2, f1
    const.s   f3, 0
    maddn.s   f3, f2, f2
    nexp01.s  f4, f1
    const.s   f0, 3
    addexp.s  f4, f0
    maddn.s   f0, f3, f4
    nexp01.s  f3, f1
    neg.s     f5, f3
    maddn.s   f2, f0, f2
    const.s   f0, 0
    const.s   f6, 0
    const.s   f7, 0
    maddn.s   f0, f5, f2
    maddn.s   f6, f2, f4
    const.s   f4, 3
    maddn.s   f7, f4, f2
    maddn.s   f3, f0, f0
    maddn.s   f4, f6, f2
    neg.s     f2, f7
    maddn.s   f0, f3, f2
    maddn.s   f7, f4, f7
    mksadj.s  f2, f1
    nexp01.s  f1, f1
    maddn.s   f1, f0, f0
    neg.s     f3, f7
    addexpm.s f0, f2
    addexp.s  f3, f2
    divn.s    f0, f1, f3
    rfr       a2, f0
    ret
    .size   sqrtf, . - sqrtf
#endif
