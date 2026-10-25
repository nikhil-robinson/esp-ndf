#include "soc_caps.h"
#if SOC_CPU_HAS_PIE
/* Float kernels on the FPU fed by PIE 128-bit loads/stores (EE.LDF.128 / EE.STF.128: four floats into four FPU
 * registers per instruction, 16-byte aligned). CALL0 ABI. FPU: 4-cycle result latency, 1/cycle issue; loads are
 * issued two groups ahead so no add/mul waits. Called from components/lib/dsp_filt.c with aligned pointers and the block
 * counts noted; the C side does tails. */
    .section .iram.text, "ax"
    .literal_position

/* void dsp_vop8_f32(const float *a, const float *b, float *y, int blocks8)  - y = a OP b, 8 floats per block */
.macro VOP name, op
    .global \name
    .align 4
\name:
    beqz        a5, 2f
    wsr         a5, lbeg                  /* S3 quirk: LBEG must change before LOOP */
    loopnez     a5, 1f
    ee.ldf.128.ip f3, f2, f1, f0, a2, 16
    ee.ldf.128.ip f7, f6, f5, f4, a3, 16
    ee.ldf.128.ip f11, f10, f9, f8, a2, 16
    ee.ldf.128.ip f15, f14, f13, f12, a3, 16
    \op         f0, f0, f4
    \op         f1, f1, f5
    \op         f2, f2, f6
    \op         f3, f3, f7
    \op         f8, f8, f12
    \op         f9, f9, f13
    \op         f10, f10, f14
    \op         f11, f11, f15
    ee.stf.128.ip f3, f2, f1, f0, a4, 16
    ee.stf.128.ip f11, f10, f9, f8, a4, 16
1:
2:  ret
.endm
    VOP dsp_vadd8_f32, add.s
    VOP dsp_vsub8_f32, sub.s
    VOP dsp_vmul8_f32, mul.s

/* void dsp_vmulc8_f32(const float *a, float *y, int blocks8, float c)  (c in a5's bits: passed as uint32) */
    .global dsp_vmulc8_f32
    .align 4
dsp_vmulc8_f32:
    beqz        a4, 2f
    wfr         f12, a5
    wsr         a4, lbeg
    loopnez     a4, 1f
    ee.ldf.128.ip f3, f2, f1, f0, a2, 16
    ee.ldf.128.ip f7, f6, f5, f4, a2, 16
    mul.s       f0, f0, f12
    mul.s       f1, f1, f12
    mul.s       f2, f2, f12
    mul.s       f3, f3, f12
    mul.s       f4, f4, f12
    mul.s       f5, f5, f12
    mul.s       f6, f6, f12
    mul.s       f7, f7, f12
    ee.stf.128.ip f3, f2, f1, f0, a3, 16
    ee.stf.128.ip f7, f6, f5, f4, a3, 16
1:
2:  ret

/* void dsp_mm4_f32(const float *A, const float *B, float *C, int m, int n, int k)
 * C[m][k] = A[m][n] B[n][k]; n, k multiples of 4, A/B/C 16-byte aligned. One row of A x 4 columns of B per tile:
 * per 4 steps of n one A load (4 floats), four B loads (4 floats each), 16 MACs (ESP-DSP's scheme), the next
 * A/B group loaded while the current one is multiplied. a8 A row ptr, a9 B col ptr, a10 4*k, a11 n/4 */
    .global dsp_mm4_f32
    .align 4
dsp_mm4_f32:
    slli        a10, a7, 2                /* row stride of B and C in bytes */
    srli        a11, a6, 2                /* n / 4 */
    addi        a1, a1, -16               /* a12-a15 are callee-saved */
    s32i        a12, a1, 0
    s32i        a13, a1, 4
    s32i        a14, a1, 8
    s32i        a15, a1, 12
    slli        a12, a6, 2                /* row stride of A in bytes */
    mov         a15, a2                   /* A row 0 */
    mov         a14, a4                   /* C row 0 */
.Lrow:
    movi        a13, 0                    /* column byte offset j*4 */
.Lcol:
    mov         a8, a15
    add         a9, a3, a13
    ee.ldf.128.ip f11, f10, f9, f8, a8, 16       /* A[i][0..3] */
    ee.ldf.128.xp f7, f6, f5, f4, a9, a10        /* B[0][j..j+3] */
    ee.ldf.128.xp f15, f14, f13, f12, a9, a10    /* B[1] */
    mul.s       f0, f4, f8                /* first group: mul, no zeroing */
    mul.s       f1, f5, f8
    mul.s       f2, f6, f8
    mul.s       f3, f7, f8
    ee.ldf.128.xp f7, f6, f5, f4, a9, a10        /* B[2] */
    madd.s      f0, f12, f9
    madd.s      f1, f13, f9
    madd.s      f2, f14, f9
    madd.s      f3, f15, f9
    ee.ldf.128.xp f15, f14, f13, f12, a9, a10    /* B[3] */
    madd.s      f0, f4, f10
    madd.s      f1, f5, f10
    madd.s      f2, f6, f10
    madd.s      f3, f7, f10
    madd.s      f0, f12, f11
    madd.s      f1, f13, f11
    madd.s      f2, f14, f11
    madd.s      f3, f15, f11
    addi        a2, a11, -1               /* remaining groups of 4 */
    beqz        a2, 3f
    ee.ldf.128.ip f11, f10, f9, f8, a8, 16
    ee.ldf.128.xp f7, f6, f5, f4, a9, a10
    addi        a2, a2, -1                /* pipelined groups, the last one peeled (no loads past the end) */
    wsr         a2, lbeg
    loopnez     a2, 1f
    ee.ldf.128.xp f15, f14, f13, f12, a9, a10    /* B[p+1] while B[p] is used */
    madd.s      f0, f4, f8
    madd.s      f1, f5, f8
    madd.s      f2, f6, f8
    madd.s      f3, f7, f8
    ee.ldf.128.xp f7, f6, f5, f4, a9, a10        /* B[p+2] */
    madd.s      f0, f12, f9
    madd.s      f1, f13, f9
    madd.s      f2, f14, f9
    madd.s      f3, f15, f9
    ee.ldf.128.xp f15, f14, f13, f12, a9, a10    /* B[p+3] */
    madd.s      f0, f4, f10
    madd.s      f1, f5, f10
    madd.s      f2, f6, f10
    madd.s      f3, f7, f10
    ee.ldf.128.xp f7, f6, f5, f4, a9, a10        /* B[p+4]: next group */
    madd.s      f0, f12, f11
    madd.s      f1, f13, f11
    madd.s      f2, f14, f11
    madd.s      f3, f15, f11
    ee.ldf.128.ip f11, f10, f9, f8, a8, 16       /* A[i][p+4..]: after f11's last use */
1:  ee.ldf.128.xp f15, f14, f13, f12, a9, a10    /* last group */
    madd.s      f0, f4, f8
    madd.s      f1, f5, f8
    madd.s      f2, f6, f8
    madd.s      f3, f7, f8
    ee.ldf.128.xp f7, f6, f5, f4, a9, a10
    madd.s      f0, f12, f9
    madd.s      f1, f13, f9
    madd.s      f2, f14, f9
    madd.s      f3, f15, f9
    ee.ldf.128.xp f15, f14, f13, f12, a9, a10
    madd.s      f0, f4, f10
    madd.s      f1, f5, f10
    madd.s      f2, f6, f10
    madd.s      f3, f7, f10
    madd.s      f0, f12, f11
    madd.s      f1, f13, f11
    madd.s      f2, f14, f11
    madd.s      f3, f15, f11
3:  add         a9, a14, a13
    ee.stf.128.ip f3, f2, f1, f0, a9, 16
    addi        a13, a13, 16
    blt         a13, a10, .Lcol
    add         a15, a15, a12
    add         a14, a14, a10
    addi        a5, a5, -1
    bnez        a5, .Lrow
    l32i        a12, a1, 0
    l32i        a13, a1, 4
    l32i        a14, a1, 8
    l32i        a15, a1, 12
    addi        a1, a1, 16
    ret

/* void dsp_biquad_f32_asm(const float *x, float *y, int n, const float *c, float *w)  - DF2 as ESP-DSP:
 * d = x - a1 w0 - a2 w1; y = b0 d + b1 w0 + b2 w1. Four samples per iteration, hand-scheduled for the in-order
 * FPU (4-cycle latency); the loop-carried chain is one msub.s per sample. n % 4 == 0. */
    .global dsp_biquad_f32_asm
    .align 4
dsp_biquad_f32_asm:
    lsi         f8, a5, 0                 /* b0 */
    lsi         f9, a5, 4                 /* b1 */
    lsi         f10, a5, 8                /* b2 */
    lsi         f11, a5, 12               /* a1 */
    lsi         f12, a5, 16               /* a2 */
    lsi         f0, a6, 0                 /* w0 */
    lsi         f1, a6, 4                 /* w1 */
    srli        a7, a4, 2
    beqz        a7, 2f
    wsr         a7, lbeg
    loopnez     a7, 1f
    /* state w0 = f0, w1 = f1; x0..x3 -> d0..d3 in f2..f5; y accumulators f6, f7, f13, f14; scheduled so every
     * FPU result is used >= 4 instructions later where the recursion allows */
    lsip        f2, a2, 4
    lsip        f3, a2, 4
    lsip        f4, a2, 4
    lsip        f5, a2, 4
    msub.s      f2, f12, f1               /* t0 = x0 - a2 w1 */
    msub.s      f3, f12, f0               /* t1 = x1 - a2 w0 */
    mul.s       f6, f9, f0                /* y0 = b1 w0 */
    mul.s       f7, f10, f0               /* y1 = b2 w0 */
    msub.s      f2, f11, f0               /* d0 = t0 - a1 w0 */
    madd.s      f6, f10, f1               /* y0 += b2 w1 */
    nop
    nop
    msub.s      f3, f11, f2               /* d1 = t1 - a1 d0 */
    msub.s      f4, f12, f2               /* t2 = x2 - a2 d0 */
    madd.s      f6, f8, f2                /* y0 += b0 d0 */
    madd.s      f7, f9, f2                /* y1 += b1 d0 */
    mul.s       f13, f10, f2              /* y2 = b2 d0 */
    msub.s      f4, f11, f3               /* d2 = t2 - a1 d1 */
    msub.s      f5, f12, f3               /* t3 = x3 - a2 d1 */
    madd.s      f7, f8, f3                /* y1 += b0 d1 */
    madd.s      f13, f9, f3               /* y2 += b1 d1 */
    mul.s       f14, f10, f3              /* y3 = b2 d1 */
    ssip        f6, a3, 4
    msub.s      f5, f11, f4               /* d3 = t3 - a1 d2 */
    madd.s      f13, f8, f4               /* y2 += b0 d2 */
    madd.s      f14, f9, f4               /* y3 += b1 d2 */
    ssip        f7, a3, 4
    mov.s       f1, f4                    /* w1 = d2 */
    mov.s       f0, f5                    /* w0 = d3 */
    madd.s      f14, f8, f5               /* y3 += b0 d3 */
    ssip        f13, a3, 4
    nop
    nop
    nop
    ssip        f14, a3, 4
1:
2:  ssi         f0, a6, 0
    ssi         f1, a6, 4
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* void dsp_r4pass_f32(float *p, int h, const float *tw, int st_bytes)
 * One radix-4 DIT group of fft_core (components/lib/dsp_fft.c): for k in [0, h): a = p[k], b = p[k+h], c = p[k+2h], d = p[k+3h]
 * (complex), b *= W^2k, c *= W^k, d *= W^3k with W^k = tw[k * st] (st_bytes = 8 st), then the radix-4 sums.
 * EE.LDF.64 / EE.STF.64 move a complex value in one instruction; the three complex multiplies are interleaved so
 * every FPU result is consumed >= 4 instructions later. */
    .global dsp_r4pass_f32
    .align 4
dsp_r4pass_f32:
    addi        a1, a1, -16
    s32i        a12, a1, 0
    s32i        a13, a1, 4
    slli        a6, a3, 3                 /* 8h: bytes between the four quarters */
    add         a7, a2, a6                /* pb */
    add         a8, a7, a6                /* pc */
    add         a9, a8, a6                /* pd */
    mov         a10, a4                   /* W^k  ptr, step st */
    mov         a11, a4                   /* W^2k ptr, step 2 st */
    mov         a12, a4                   /* W^3k ptr, step 3 st */
    slli        a13, a5, 1                /* 2 st */
    add         a6, a13, a5               /* 3 st */
    wsr         a3, lbeg
    loopnez     a3, 1f
    ee.ldf.64.ip f11, f10, a11, 0         /* w2 */
    ee.ldf.64.ip f3, f2, a7, 0            /* b */
    ee.ldf.64.ip f9, f8, a10, 0           /* w1 */
    ee.ldf.64.ip f5, f4, a8, 0            /* c */
    ee.ldf.64.ip f13, f12, a12, 0         /* w3 */
    ee.ldf.64.ip f7, f6, a9, 0            /* d */
    add         a11, a11, a13
    add         a10, a10, a5
    add         a12, a12, a6
    mul.s       f0, f2, f10               /* br w2r */
    mul.s       f1, f2, f11               /* br w2i */
    mul.s       f14, f4, f8               /* cr w1r */
    mul.s       f15, f4, f9               /* cr w1i */
    msub.s      f0, f3, f11               /* b2r = br w2r - bi w2i */
    madd.s      f1, f3, f10               /* b2i = br w2i + bi w2r */
    mul.s       f2, f6, f12               /* dr w3r */
    mul.s       f3, f6, f13               /* dr w3i */
    msub.s      f14, f5, f9               /* c1r */
    madd.s      f15, f5, f8               /* c1i */
    msub.s      f2, f7, f13               /* d3r */
    madd.s      f3, f7, f12               /* d3i */
    ee.ldf.64.ip f5, f4, a2, 0            /* a */
    add.s       f6, f4, f0                /* t0 = a + b2 */
    add.s       f7, f5, f1
    sub.s       f4, f4, f0                /* t1 = a - b2 */
    sub.s       f5, f5, f1
    add.s       f0, f14, f2               /* t2 = c1 + d3 */
    add.s       f1, f15, f3
    sub.s       f14, f14, f2              /* t3 = c1 - d3 */
    sub.s       f15, f15, f3
    add.s       f2, f6, f0                /* X0 = t0 + t2 */
    add.s       f3, f7, f1
    sub.s       f6, f6, f0                /* X2 = t0 - t2 */
    sub.s       f7, f7, f1
    add.s       f0, f4, f15               /* X1 = t1 - j t3 */
    sub.s       f1, f5, f14
    sub.s       f4, f4, f15               /* X3 = t1 + j t3 */
    add.s       f5, f5, f14
    ee.stf.64.ip f3, f2, a2, 8
    ee.stf.64.ip f7, f6, a8, 8
    ee.stf.64.ip f1, f0, a7, 8
    ee.stf.64.ip f5, f4, a9, 8
1:  l32i        a12, a1, 0
    l32i        a13, a1, 4
    addi        a1, a1, 16
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* void dsp_r4first_f32(float *x, int groups) - the first radix-4 pass (h = 1, all twiddles 1) over consecutive
 * 4-point groups; x 16-byte aligned. 2 x 128-bit loads, 16 adds, 2 x 128-bit stores per group. */
    .global dsp_r4first_f32
    .align 4
dsp_r4first_f32:
    mov         a4, a2
    wsr         a3, lbeg
    loopnez     a3, 1f
    ee.ldf.128.ip f3, f2, f1, f0, a2, 16  /* a, b */
    ee.ldf.128.ip f7, f6, f5, f4, a2, 16  /* c, d */
    add.s       f8, f0, f2                /* t0 = a + b */
    add.s       f9, f1, f3
    sub.s       f10, f0, f2               /* t1 = a - b */
    sub.s       f11, f1, f3
    add.s       f12, f4, f6               /* t2 = c + d */
    add.s       f13, f5, f7
    sub.s       f14, f4, f6               /* t3 = c - d */
    sub.s       f15, f5, f7
    add.s       f0, f8, f12               /* X0 */
    add.s       f1, f9, f13
    add.s       f2, f10, f15              /* X1 = t1 - j t3 */
    sub.s       f3, f11, f14
    sub.s       f4, f8, f12               /* X2 */
    sub.s       f5, f9, f13
    sub.s       f6, f10, f15              /* X3 = t1 + j t3 */
    add.s       f7, f11, f14
    ee.stf.128.ip f3, f2, f1, f0, a4, 16
    ee.stf.128.ip f7, f6, f5, f4, a4, 16
1:  ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* void dsp_fft_s16_stage(int16_t *x, int n2, int blocks, const int16_t *wbr)  - one radix-2 stage of the int16
 * FFT (natural-order input, bit-reversed twiddles, as ESP-DSP dsps_fft2r_sc16): for each block j, w = wbr[j]
 * (cos, sin Q15) broadcast; top' = (top + bot conj w) / 2, bot' = (top - bot conj w) / 2, 4 complex per vector
 * step (EE.CMUL.S16 modes 2 + 3, SAR = 16). n2 % 4 == 0, x 16-byte aligned. */
    .global dsp_fft_s16_stage
    .align 4
dsp_fft_s16_stage:
    addi        a1, a1, -16
    movi        a6, 32767
    s16i        a6, a1, 0
    mov         a6, a1
    ee.vldbc.16.ip q6, a6, 0              /* 32767 in every lane: x * 32767 >> 16 = x / 2 */
    movi        a6, 16
    wsr         a6, sar
    srli        a7, a3, 2                 /* vector steps per block */
    slli        a6, a3, 2                 /* n2 complex = 4 n2 bytes */
    mov         a9, a2                    /* top */
1:  add         a8, a9, a6                /* bottom */
    ee.vldbc.32.ip q5, a5, 4              /* w for this block */
    wsr         a7, lbeg
    loopnez     a7, 2f
    ee.vld.128.ip q0, a8, 0
    ee.vld.128.ip q2, a9, 0
    ee.cmul.s16 q1, q5, q0, 2
    ee.vmul.s16 q2, q2, q6
    ee.cmul.s16 q1, q5, q0, 3
    ee.vsubs.s16 q3, q2, q1
    ee.vadds.s16.st.incp q3, a8, q3, q2, q1   /* store bottom, q3 = top */
    ee.vst.128.ip q3, a9, 16
2:  add         a9, a9, a6                /* next block: skip its bottom half */
    addi        a4, a4, -1
    bnez        a4, 1b
    addi        a1, a1, 16
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* void dsp_fft_s16_last2(int16_t *x, int n, const int16_t *wbr) - the last two stages (half-blocks of 2, then 1
 * complex) of dsp_fft_s16, vectorised as ESP-DSP: 64-bit half loads / unzip / zip. n % 8 == 0, x 16-aligned. */
    .global dsp_fft_s16_last2
    .align 4
dsp_fft_s16_last2:
    addi        a1, a1, -16
    movi        a6, 32767
    s16i        a6, a1, 0
    mov         a6, a1
    ee.vldbc.16.ip q6, a6, 0
    movi        a6, 16
    wsr         a6, sar
    /* stage n2 = 2: blocks n/4, twiddles wbr[0 .. n/4), two blocks (8 complex) per step */
    srli        a7, a3, 3
    mov         a8, a2
    mov         a9, a4
    wsr         a7, lbeg
    loopnez     a7, 1f
    ee.vld.l.64.ip q0, a8, 8              /* top of block 0 */
    ee.vldbc.32.ip q2, a9, 4              /* w block 0 */
    ee.vld.l.64.ip q1, a8, 8              /* bottom of block 0 */
    ee.vldbc.32.ip q3, a9, 4              /* w block 1 */
    ee.vld.h.64.ip q0, a8, 8              /* top of block 1 */
    ee.vunzip.32 q2, q3                   /* q2 = w0 w0 w1 w1 */
    ee.vld.h.64.ip q1, a8, -24            /* bottom of block 1, back to the start */
    ee.vmul.s16 q0, q0, q6
    ee.cmul.s16 q4, q2, q1, 2
    ee.cmul.s16 q4, q2, q1, 3
    ee.vadds.s16 q2, q0, q4
    ee.vsubs.s16 q3, q0, q4
    ee.vst.l.64.ip q2, a8, 8
    ee.vst.l.64.ip q3, a8, 8
    ee.vst.h.64.ip q2, a8, 8
    ee.vst.h.64.ip q3, a8, 8
1:  /* stage n2 = 1: blocks n/2, twiddles wbr[0 .. n/2), four blocks (8 complex) per step */
    srli        a7, a3, 3
    mov         a8, a2
    mov         a9, a4
    wsr         a7, lbeg
    loopnez     a7, 2f
    ee.vld.128.ip q0, a8, 16
    ee.vld.128.ip q1, a8, -16
    ee.vld.128.ip q2, a9, 16              /* w for 4 blocks */
    ee.vunzip.32 q0, q1                   /* q0 = tops, q1 = bottoms */
    ee.cmul.s16 q3, q2, q1, 2
    ee.vmul.s16 q0, q0, q6
    ee.cmul.s16 q3, q2, q1, 3
    ee.vsubs.s16 q1, q0, q3
    ee.vadds.s16 q0, q0, q3
    ee.vzip.32  q0, q1
    ee.vst.128.ip q0, a8, 16
    ee.vst.128.ip q1, a8, 16
2:  addi        a1, a1, 16
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* void dsp_vadds16_sh(const int16_t *a, const int16_t *b, int16_t *y, int blocks16, int shift)
 * y = sat(a + b) >> shift (vadds then vmul by 1 with SAR = shift). 16 values per block, all 16-aligned. */
    .global dsp_vadds16_sh
    .align 4
dsp_vadds16_sh:                           /* 8 values per block here */
    beqz        a5, 2f
    ee.vld.128.ip q0, a2, 16
    bnez        a6, 3f
    wsr         a5, lbeg                  /* shift 0: fused add + next load, 3 instructions per 8 values */
    loopnez     a5, 1f
    ee.vld.128.ip q1, a3, 16
    ee.vadds.s16.ld.incp q0, a2, q4, q0, q1
    ee.vst.128.ip q4, a4, 16
1:  ret
3:  wsr         a6, sar
    addi        a1, a1, -16
    movi        a7, 1
    s16i        a7, a1, 0
    mov         a7, a1
    ee.vldbc.16.ip q7, a7, 0              /* ones: x * 1 >> sar */
    addi        a1, a1, 16
    wsr         a5, lbeg
    loopnez     a5, 4f
    ee.vld.128.ip q1, a3, 16
    ee.vadds.s16.ld.incp q0, a2, q4, q0, q1
    ee.vmul.s16 q4, q4, q7
    ee.vst.128.ip q4, a4, 16
4:
2:  ret

/* void dsp_vmuls16_sh(const int16_t *a, const int16_t *b, int16_t *y, int blocks16, int shift) - y = sat(a b >> shift) */
    .global dsp_vmuls16_sh
    .align 4
dsp_vmuls16_sh:
    beqz        a5, 2f
    wsr         a6, sar
    wsr         a5, lbeg
    loopnez     a5, 1f
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a3, 16
    ee.vld.128.ip q2, a2, 16
    ee.vld.128.ip q3, a3, 16
    ee.vmul.s16 q4, q0, q1
    ee.vmul.s16 q5, q2, q3
    ee.vst.128.ip q4, a4, 16
    ee.vst.128.ip q5, a4, 16
1:
2:  ret

/* void dsp_mm8_s16(const int16_t *A, const int16_t *B, int16_t *C, int m, int n, int k) - shift at [a1+0]
 * C[i][j..j+7] = sat(sum_p A[i][p] B[p][j..j+7] >> shift): A element broadcast, MAC into the 8 QACC lanes (40-bit),
 * next broadcast fused into the MAC (EE.VMULAS.S16.QACC.LDBC.INCP). k % 8 == 0, B/C 16-aligned. */
    .global dsp_mm8_s16
    .align 4
dsp_mm8_s16:
    l32i        a8, a1, 0                 /* shift */
    addi        a1, a1, -16
    s32i        a12, a1, 0
    s32i        a13, a1, 4
    s32i        a14, a1, 8
    s32i        a15, a1, 12
    slli        a9, a7, 1                 /* B / C row stride (bytes) */
    slli        a10, a6, 1                /* A row stride */
    srli        a11, a6, 1                /* pairs of MACs (ping-pong B registers, as ESP-DSP); n odd: one extra first */
    mov         a12, a2                   /* A row */
.Lmr:
    movi        a13, 0                    /* column byte offset */
.Lmc:
    mov         a14, a12
    add         a15, a3, a13
    ee.zero.qacc
    ee.vldbc.16.ip q1, a14, 2             /* A[i][0] */
    ee.vld.128.xp q0, a15, a9             /* B[0][j..] */
    bbci        a6, 0, 2f
    ee.vmulas.s16.qacc.ldbc.incp q1, a14, q0, q1   /* odd n: first MAC alone */
    ee.vld.128.xp q0, a15, a9
2:  wsr         a11, lbeg
    loopnez     a11, 1f
    ee.vld.128.xp q2, a15, a9             /* B[p+1] */
    ee.vmulas.s16.qacc.ldbc.incp q1, a14, q0, q1   /* QACC += B[p] * A[i][p]; q1 = A[i][p+1] */
    ee.vld.128.xp q0, a15, a9             /* B[p+2] (one row past the end on the last pass: never used) */
    ee.vmulas.s16.qacc.ldbc.incp q1, a14, q2, q1
1:  ee.srcmb.s16.qacc q3, a8, 0           /* lanes >> shift, saturated to int16 */
    add         a15, a4, a13
    ee.vst.128.ip q3, a15, 0
    addi        a13, a13, 16
    blt         a13, a9, .Lmc
    add         a12, a12, a10
    add         a4, a4, a9
    addi        a5, a5, -1
    bnez        a5, .Lmr
    l32i        a12, a1, 0
    l32i        a13, a1, 4
    l32i        a14, a1, 8
    l32i        a15, a1, 12
    addi        a1, a1, 16
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* int32_t dsp_dotu_s16(const int16_t *x, const int16_t *h, int groups8) - sum x[i] h[i], x only 2-byte aligned
 * (EE.LD.128.USAR + EE.SRC.Q realign), h 16-aligned, 40-bit ACCX, low 32 bits returned. Reads up to 16 bytes past
 * the end of x. */
    .global dsp_dotu_s16
    .align 4
dsp_dotu_s16:
    ee.zero.accx
    ee.ld.128.usar.ip q0, a2, 16
    beqz        a4, 2f
    wsr         a4, lbeg
    loopnez     a4, 1f
    ee.ld.128.usar.ip q1, a2, 16
    ee.vld.128.ip q3, a3, 16
    ee.src.q    q2, q0, q1
    mv.qr       q0, q1
    ee.vmulas.s16.accx q2, q3
1:
2:  movi        a5, 0
    ee.srs.accx a2, a5, 0
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* void dsp_fird_core_s16(const int16_t *x0, const int16_t *hr, int16_t *out, int n_out, int step_bytes, int groups8)
 * stack [a1+0] = shift, [a1+4] = rounding: out[i] = clamp16((dot(x0 + i step, hr) + rnd) >> shift). */
    .global dsp_fird_core_s16
    .align 4
dsp_fird_core_s16:
    l32i        a10, a1, 0                /* shift */
    l32i        a11, a1, 4                /* rounding */
    beqz        a5, 9f
    ssr         a10                       /* SAR for sra */
.Lfo:
    mov         a8, a2
    mov         a9, a3
    ee.zero.accx
    ee.ld.128.usar.ip q0, a8, 16
    wsr         a7, lbeg
    loopnez     a7, 1f
    ee.ld.128.usar.ip q1, a8, 16
    ee.vld.128.ip q3, a9, 16
    ee.src.q    q2, q0, q1
    mv.qr       q0, q1
    ee.vmulas.s16.accx q2, q3
1:  movi        a9, 0
    ee.srs.accx a9, a9, 0
    add         a9, a9, a11
    ssr         a10
    sra         a9, a9
    clamps      a9, a9, 15
    s16i        a9, a4, 0
    addi        a4, a4, 2
    add         a2, a2, a6
    addi        a5, a5, -1
    bnez        a5, .Lfo
9:  ret
#endif
