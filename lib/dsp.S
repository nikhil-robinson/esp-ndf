#include "soc_caps.h"
#if SOC_CPU_HAS_PIE
/* Vector (PIE, coprocessor 3) and FPU kernels. CALL0 ABI: args a2..a7,
 * result a2, a2-a11 scratch. Vector loads/stores need 16-byte aligned
 * pointers (the low 4 address bits are ignored by EE.VLD/VST.128).
 * Every loop is preceded by "wsr <count>, lbeg": the S3 runs a re-entered
 * zero-overhead loop at half speed if LBEG still points at it (measured). */
    .section .iram.text, "ax"
    .literal_position

/* int32 dsp_dot_s16(const int16_t *a, const int16_t *b, int n)
 * n: multiple of 16, >= 16. 40-bit accumulator, result saturated to 32 bits.
 * Software pipelined: every operand is loaded two instructions before the
 * multiply that uses it (no load-use stall); prologue/epilogue keep all
 * loads inside the buffers. */
    .global dsp_dot_s16
    .align 4
dsp_dot_s16:
    ee.zero.accx
    srli        a5, a4, 4                 /* pairs of 8-element blocks */
    beqz        a5, 2f
    ee.vld.128.ip q0, a2, 16              /* a0 */
    ee.vld.128.ip q2, a3, 16              /* b0 */
    addi        a5, a5, -1
    wsr         a5, lbeg                  /* S3 quirk: LBEG must change (see Makefile) */
    loopnez     a5, 1f
    ee.vld.128.ip q3, a3, 16                          /* b(k+1) */
    ee.vmulas.s16.accx.ld.ip q1, a2, 16, q0, q2       /* acc += a(k)*b(k); a(k+1) */
    ee.vld.128.ip q2, a3, 16                          /* b(k+2) */
    ee.vmulas.s16.accx.ld.ip q0, a2, 16, q1, q3       /* acc += a(k+1)*b(k+1); a(k+2) */
1:  ee.vld.128.ip q3, a3, 16                          /* last pair, no further loads */
    ee.vmulas.s16.accx.ld.ip q1, a2, 16, q0, q2
    ee.vmulas.s16.accx q1, q3
2:  movi        a6, 0
    ee.srs.accx a2, a6, 0
    ret

/* void dsp_add_s16_sat(int16_t *dst, const int16_t *a, const int16_t *b, int n)
 * dst = sat16(a + b); n multiple of 16, >= 16. Software pipelined: one load
 * folds into each add (EE.VADDS.S16.LD.INCP), operands are loaded well
 * before use, results are stored one step later. No over-read. */
    .global dsp_add_s16_sat
    .align 4
dsp_add_s16_sat:
    srli        a6, a5, 4
    beqz        a6, 2f
    ee.vld.128.ip q0, a3, 16              /* a0 */
    ee.vld.128.ip q1, a4, 16              /* b0 */
    addi        a6, a6, -1
    wsr         a6, lbeg                  /* S3 quirk: LBEG must change (see Makefile) */
    loopnez     a6, 1f
    ee.vld.128.ip q3, a4, 16                          /* b(k+1) */
    ee.vadds.s16.ld.incp q2, a3, q4, q0, q1           /* r(k) = a(k)+b(k); a(k+1) */
    ee.vld.128.ip q1, a4, 16                          /* b(k+2) */
    ee.vst.128.ip q4, a2, 16                          /* store r(k) */
    ee.vadds.s16.ld.incp q0, a3, q5, q2, q3           /* r(k+1); a(k+2) */
    ee.vst.128.ip q5, a2, 16
1:  ee.vld.128.ip q3, a4, 16                          /* last pair */
    ee.vadds.s16.ld.incp q2, a3, q4, q0, q1
    ee.vst.128.ip q4, a2, 16
    ee.vadds.s16 q5, q2, q3
    ee.vst.128.ip q5, a2, 16
2:  ret

/* void dsp_copy128(void *dst, const void *src, size_t n)   n multiple of 32 */
    .global dsp_copy128
    .align 4
dsp_copy128:
    srli        a5, a4, 5
    wsr         a5, lbeg            /* S3 quirk: LBEG must change (see Makefile) */
    loopnez     a5, 1f
    ee.vld.128.ip q0, a3, 16
    ee.vld.128.ip q1, a3, 16
    ee.vst.128.ip q0, a2, 16
    ee.vst.128.ip q1, a2, 16
1:  ret

/* float dsp_dot_f32(const float *a, const float *b, int n)   n multiple of 4
 * 128-bit loads straight into FPU registers (EE.LDF.128.IP), 4 independent
 * fused multiply-add chains. a, b 16-byte aligned. */
    .global dsp_dot_f32
    .align 4
dsp_dot_f32:
    movi        a5, 0
    wfr         f8, a5
    mov.s       f9, f8
    mov.s       f10, f8
    mov.s       f11, f8
    srli        a5, a4, 2
    wsr         a5, lbeg            /* S3 quirk: LBEG must change (see Makefile) */
    loopnez     a5, 1f
    ee.ldf.128.ip f3, f2, f1, f0, a2, 16
    ee.ldf.128.ip f7, f6, f5, f4, a3, 16
    madd.s      f8, f0, f4
    madd.s      f9, f1, f5
    madd.s      f10, f2, f6
    madd.s      f11, f3, f7
1:  add.s       f8, f8, f9
    add.s       f10, f10, f11
    add.s       f8, f8, f10
    rfr         a2, f8
    ret

/* void dsp_set128(void *dst, const void *pattern16, size_t n)   n multiple of 32 */
    .global dsp_set128
    .align 4
dsp_set128:
    ee.vld.128.ip q0, a3, 0
    srli        a5, a4, 5
    wsr         a5, lbeg            /* S3 quirk: LBEG must change (see Makefile) */
    loopnez     a5, 1f
    ee.vst.128.ip q0, a2, 16
    ee.vst.128.ip q0, a2, 16
1:  ret

/* void dsp_copy128u(void *dst, const void *src, size_t n)
 * dst 16-byte aligned, src any alignment, n multiple of 48. One unaligned load
 * sets SAR_BYTE, then fused shift+load (EE.SRC.Q.LD.IP) with three rotating
 * registers: 2 instructions per 16 bytes. Reads up to 32 bytes beyond
 * src + n (the caller keeps that inside RAM). */
    .global dsp_copy128u
    .align 4
dsp_copy128u:
    movi        a6, 48
    quou        a5, a4, a6
    beqz        a5, 2f
    ee.ld.128.usar.ip q0, a3, 16          /* block 0, SAR_BYTE = src & 15 */
    ee.vld.128.ip q1, a3, 16              /* block 1 */
    wsr         a5, lbeg                  /* S3 quirk: LBEG must change (see Makefile) */
    loopnez     a5, 1f
    ee.src.q.ld.ip q2, a3, 16, q0, q1     /* q0 = out k,   q2 = block k+2 */
    ee.vst.128.ip q0, a2, 16
    ee.src.q.ld.ip q0, a3, 16, q1, q2     /* q1 = out k+1, q0 = block k+3 */
    ee.vst.128.ip q1, a2, 16
    ee.src.q.ld.ip q1, a3, 16, q2, q0     /* q2 = out k+2, q1 = block k+4 */
    ee.vst.128.ip q2, a2, 16
1:
2:  ret
#endif /* SOC_CPU_HAS_PIE */
