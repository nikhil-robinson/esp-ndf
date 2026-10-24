#include "soc_caps.h"
#if SOC_CPU_HAS_PIE
/* int32_t nn_dot_s8(const int8_t *a, const int8_t *b, int n16) - sum a[i] b[i] over 16 n16 bytes; a any alignment
 * (EE.LD.128.USAR + EE.SRC.Q), b 16-aligned; 16 MACs per EE.VMULAS.S8.ACCX. Reads up to 16 bytes past a. */
    .section .iram.text, "ax"
    .global nn_dot_s8
    .align 4
nn_dot_s8:
    ee.zero.accx
    ee.ld.128.usar.ip q0, a2, 16
    wsr         a4, lbeg
    loopnez     a4, 1f
    ee.ld.128.usar.ip q1, a2, 16
    ee.vld.128.ip q3, a3, 16
    ee.src.q    q2, q0, q1
    mv.qr       q0, q1
    ee.vmulas.s8.accx q2, q3
1:  movi        a5, 0
    ee.srs.accx a2, a5, 0
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* void nn_conv_row_s8(const int8_t *row, int k16, const int8_t *w, int oc, const nn_chq_t *q, int8_t *out)
 * For every output channel o: acc = dot(row[0..16 k16), w[o]) + q[o].bias, then TFLite requantisation (right
 * shifts only: the C side routes left shifts elsewhere) with q[o] = { bias, mult, rsh, rhalf, negm }:
 * r = floor((acc m + 2^30) / 2^31); y = (r + rhalf + (r < 0 ? negm : 0)) >> rsh, which equals TFLite's
 * round-half-away divide exactly; + out_off, clamp, store. Constants out_off, act_min, act_max at q[-1].
 * row and w 16-aligned. */
#define Q_BIAS   0
#define Q_MULT   4
#define Q_RSH    8
#define Q_RHALF  12
#define Q_NEGM   16
#define Q_SIZE   20
.macro RQ_FAST                           /* rsh >= 1 everywhere, act = [-128, 127] */
    l32i        a9, a6, Q_MULT
    mull        a10, a8, a9
    mulsh       a11, a8, a9
    add         a10, a10, a15
    saltu       a9, a10, a15
    add         a11, a11, a9
    ssai        31
    src         a8, a11, a10              /* r */
    l32i        a9, a6, Q_RHALF
    srai        a10, a8, 31               /* -1 if r < 0 */
    add         a8, a8, a9
    add         a8, a8, a10
    l32i        a11, a6, Q_RSH
    ssr         a11
    sra         a8, a8
    add         a8, a8, a12
    clamps      a8, a8, 7
    s8i         a8, a7, 0
    addi        a7, a7, 1
    addi        a6, a6, Q_SIZE
.endm

.macro RQ_STORE
    l32i        a9, a6, Q_MULT
    mull        a10, a8, a9
    mulsh       a11, a8, a9
    add         a10, a10, a15             /* low + 2^30 */
    saltu       a9, a10, a15              /* carry out of the low word */
    add         a11, a11, a9
    ssai        31
    src         a8, a11, a10              /* r = floor((x m + 2^30) / 2^31) */
    l32i        a9, a6, Q_NEGM
    srai        a10, a8, 31
    and         a10, a10, a9              /* -1 if r < 0 and rsh > 0 */
    l32i        a9, a6, Q_RHALF
    add         a8, a8, a10
    add         a8, a8, a9
    l32i        a11, a6, Q_RSH
    ssr         a11
    sra         a8, a8
    add         a8, a8, a12               /* + out_off */
    max         a8, a8, a13
    min         a8, a8, a14
    s8i         a8, a7, 0
    addi        a7, a7, 1
    addi        a6, a6, Q_SIZE
.endm

    .global nn_conv_row_s8
    .align 4
nn_conv_row_s8:
    addi        a1, a1, -48
    s32i        a12, a1, 0
    s32i        a13, a1, 4
    s32i        a14, a1, 8
    s32i        a15, a1, 12
    l32i        a12, a6, -20              /* out_off */
    l32i        a13, a6, -16              /* act_min */
    l32i        a14, a6, -12              /* act_max */
    movi        a15, 0x40000000           /* rounding nudge 2^30 */
    bnei        a3, 1, .Lgen
    l32i        a9, a6, -8                /* flag: fast requant allowed */
    bnez        a9, .Lfast1
    /* k16 == 1: row in q7; the next channel's MAC runs while this one is requantised; zero-overhead loop */
    ee.vld.128.ip q7, a2, 0
    ee.zero.accx
    ee.vld.128.ip q1, a4, 16
    ee.vmulas.s8.accx q7, q1
    wsr         a5, lbeg
    loopnez     a5, .L1end
    movi        a9, 0
    ee.srs.accx a8, a9, 0
    ee.zero.accx
    ee.vld.128.ip q1, a4, 16              /* next channel's weights (one block past the end on the last pass) */
    ee.vmulas.s8.accx q7, q1
    l32i        a9, a6, Q_BIAS
    add         a8, a8, a9
    RQ_STORE
.L1end:
    j           .Lret
.Lfast1:                                  /* k16 == 1 with the fast requant, hand-scheduled (a2/a3 free) */
    ee.vld.128.ip q7, a2, 0
    ee.zero.accx
    ee.vld.128.ip q1, a4, 16
    ee.vmulas.s8.accx q7, q1
    wsr         a5, lbeg
    loopnez     a5, .L2end
    l32i        a9, a6, Q_MULT
    l32i        a11, a6, Q_BIAS
    movi        a10, 0
    ee.srs.accx a8, a10, 0
    ee.zero.accx
    ee.vld.128.ip q1, a4, 16              /* next channel (one block past the end on the last pass) */
    add         a8, a8, a11
    ssai        31
    mull        a10, a8, a9
    mulsh       a11, a8, a9
    ee.vmulas.s8.accx q7, q1
    l32i        a9, a6, Q_RHALF
    add         a10, a10, a15
    saltu       a2, a10, a15
    add         a11, a11, a2
    l32i        a3, a6, Q_RSH
    src         a8, a11, a10
    srai        a10, a8, 31
    add         a8, a8, a9
    ssr         a3
    add         a8, a8, a10
    addi        a6, a6, Q_SIZE
    sra         a8, a8
    add         a8, a8, a12
    clamps      a8, a8, 7
    s8i         a8, a7, 0
    addi        a7, a7, 1
.L2end:
    j           .Lret
.Lgen:                                    /* k16 >= 2: dot pipelined two blocks per step (no load-use stall) */
    l32i        a9, a6, -8
    movi        a10, 9
    bne         a3, a10, .Lgen2
    bnez        a9, .Lk9                  /* 3x3 x 16 channels with the fast requant: straight-line */
.Lgen2:
    s32i        a9, a1, 16                /* fast flag */
    movi        a10, 3
    quou        a9, a3, a10
    s32i        a9, a1, 20                /* k16 / 3 */
    remu        a11, a3, a10
    s32i        a11, a1, 24               /* k16 % 3 */
    movi        a10, 2
    sub         a11, a10, a11
    slli        a11, a11, 4
    s32i        a11, a1, 28               /* bytes of weights loaded ahead: (2 - k16 % 3) * 16 */
    s32i        a2, a1, 12+20             /* row pointer (a2 is scratch in the requant) */
.Lgo:                                     /* dot: rows and weights rotate through q0-q2 / q3-q5, each loaded two
                                           * blocks ahead; EE.VMULAS.S8.ACCX.LD.IP multiplies and loads the next row */
    ee.zero.accx
    mov         a8, a2
    ee.vld.128.ip q0, a8, 16              /* row 0 */
    ee.vld.128.ip q1, a8, 16              /* row 1 */
    ee.vld.128.ip q3, a4, 16              /* w 0 */
    ee.vld.128.ip q4, a4, 16              /* w 1 */
    l32i        a9, a1, 20                /* groups of 3 blocks */
    l32i        a11, a1, 24
    wsr         a9, lbeg
    loopnez     a9, 2f
    ee.vld.128.ip q5, a4, 16                       /* w i+2 */
    ee.vmulas.s8.accx.ld.ip q2, a8, 16, q0, q3     /* row i * w i; row i+2 */
    ee.vld.128.ip q3, a4, 16
    ee.vmulas.s8.accx.ld.ip q0, a8, 16, q1, q4
    ee.vld.128.ip q4, a4, 16
    ee.vmulas.s8.accx.ld.ip q1, a8, 16, q2, q5
2:  beqz        a11, 4f                   /* 1 or 2 blocks left (rows in q0, q1; weights in q3, q4) */
    ee.vmulas.s8.accx q0, q3
    bnei        a11, 2, 3f
    ee.vmulas.s8.accx q1, q4
3:
4:  l32i        a9, a1, 28
    sub         a4, a4, a9                /* weight blocks loaded ahead belong to the next channel */
5:  l32i        a10, a1, 16
    movi        a9, 0
    ee.srs.accx a8, a9, 0
    l32i        a9, a6, Q_MULT
    l32i        a11, a6, Q_BIAS
    beqz        a10, 6f
    add         a8, a8, a11               /* scheduled fast requant (as .Lfast1) */
    ssai        31
    mull        a10, a8, a9
    mulsh       a11, a8, a9
    l32i        a9, a6, Q_RHALF
    add         a10, a10, a15
    saltu       a2, a10, a15
    add         a11, a11, a2
    l32i        a2, a6, Q_RSH
    src         a8, a11, a10
    srai        a10, a8, 31
    add         a8, a8, a9
    ssr         a2
    add         a8, a8, a10
    addi        a6, a6, Q_SIZE
    sra         a8, a8
    add         a8, a8, a12
    clamps      a8, a8, 7
    s8i         a8, a7, 0
    addi        a7, a7, 1
    l32i        a2, a1, 12+20             /* row pointer back (saved below) */
    j           7f
6:  add         a8, a8, a11
    RQ_STORE
7:  addi        a5, a5, -1
    bnez        a5, .Lgo
.Lk9:                                     /* k16 == 9, fast requant: 9 blocks fully unrolled (a3 free) */
    ee.zero.accx
    mov         a8, a2
    ee.vld.128.ip q0, a8, 16
    ee.vld.128.ip q1, a8, 16
    ee.vld.128.ip q3, a4, 16
    ee.vld.128.ip q4, a4, 16
    .rept 3
    ee.vld.128.ip q5, a4, 16
    ee.vmulas.s8.accx.ld.ip q2, a8, 16, q0, q3
    ee.vld.128.ip q3, a4, 16
    ee.vmulas.s8.accx.ld.ip q0, a8, 16, q1, q4
    ee.vld.128.ip q4, a4, 16
    ee.vmulas.s8.accx.ld.ip q1, a8, 16, q2, q5
    .endr
    addi        a4, a4, -32               /* two weight blocks loaded ahead: next channel's */
    l32i        a9, a6, Q_MULT
    l32i        a11, a6, Q_BIAS
    movi        a10, 0
    ee.srs.accx a8, a10, 0
    add         a8, a8, a11
    ssai        31
    mull        a10, a8, a9
    mulsh       a11, a8, a9
    l32i        a9, a6, Q_RHALF
    add         a10, a10, a15
    saltu       a3, a10, a15
    add         a11, a11, a3
    l32i        a3, a6, Q_RSH
    src         a8, a11, a10
    srai        a10, a8, 31
    add         a8, a8, a9
    ssr         a3
    add         a8, a8, a10
    addi        a6, a6, Q_SIZE
    sra         a8, a8
    add         a8, a8, a12
    clamps      a8, a8, 7
    s8i         a8, a7, 0
    addi        a7, a7, 1
    addi        a5, a5, -1
    bnez        a5, .Lk9
.Lret:
    l32i        a12, a1, 0
    l32i        a13, a1, 4
    l32i        a14, a1, 8
    l32i        a15, a1, 12
    addi        a1, a1, 48
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* void nn_dw_acc16(const int8_t *const *taps, int ntaps, const int8_t *f, int fstride, uint8_t *raw)
 * QACC (16 x 20-bit lanes) = sum_t taps[t][0..15] * f[t * fstride + 0..15]; raw <- QACC_L (20 B) at 0, QACC_H at 32.
 * taps / f 16-aligned. */
    .global nn_dw_acc16
    .align 4
nn_dw_acc16:
    ee.zero.qacc
    wsr         a3, lbeg
    loopnez     a3, 1f
    l32i        a8, a2, 0
    addi        a2, a2, 4
    ee.vld.128.ip q0, a8, 0
    ee.vld.128.xp q1, a4, a5
    ee.vmulas.s8.qacc q0, q1
1:  mov         a8, a6
    ee.st.qacc_l.l.128.ip a8, 16
    ee.st.qacc_l.h.32.ip a8, 16
    ee.st.qacc_h.l.128.ip a8, 16
    ee.st.qacc_h.h.32.ip a8, 0
    ret

/* void nn_rq_s8(const int32_t *acc, int n, const nn_chq_t *q, int8_t *out, int32_t out_off) - acc[i] + q[i].bias,
 * fast requantisation (all rsh >= 1, act [-128, 127]), + out_off, out[i] */
    .global nn_rq_s8
    .align 4
nn_rq_s8:
    addi        a1, a1, -16
    s32i        a12, a1, 0
    s32i        a15, a1, 4
    mov         a12, a6
    movi        a15, 0x40000000
    mov         a6, a4
    mov         a7, a5
    wsr         a3, lbeg
    loopnez     a3, 1f
    l32i        a8, a2, 0
    l32i        a9, a6, Q_MULT
    l32i        a11, a6, Q_BIAS
    addi        a2, a2, 4
    add         a8, a8, a11
    ssai        31
    mull        a10, a8, a9
    mulsh       a11, a8, a9
    l32i        a9, a6, Q_RHALF
    add         a10, a10, a15
    saltu       a4, a10, a15
    add         a11, a11, a4
    l32i        a4, a6, Q_RSH
    src         a8, a11, a10
    srai        a10, a8, 31
    add         a8, a8, a9
    ssr         a4
    add         a8, a8, a10
    addi        a6, a6, Q_SIZE
    sra         a8, a8
    add         a8, a8, a12
    clamps      a8, a8, 7
    s8i         a8, a7, 0
    addi        a7, a7, 1
1:  l32i        a12, a1, 0
    l32i        a15, a1, 4
    addi        a1, a1, 16
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* void nn_dw_rq16(const uint8_t *raw, const nn_chq_t *q, int8_t *out, int32_t out_off)
 * raw = QACC_L (20 B) at 0, QACC_H at 32 (nn_dw_acc16); each 20-bit lane + q[i].bias -> fast requant -> out[i]. */
.macro LANE base, bit                     /* a8 = sign-extended 20-bit lane at bit offset \bit of the half at \base */
    .if ((\bit) % 32) <= 12
    l32i        a8, \base, ((\bit) / 32) * 4
    slli        a8, a8, 12 - ((\bit) % 32)
    srai        a8, a8, 12
    .else
    l32i        a9, \base, ((\bit) / 32) * 4
    l32i        a8, \base, ((\bit) / 32) * 4 + 4
    ssai        ((\bit) % 32)
    src         a8, a8, a9
    slli        a8, a8, 12
    srai        a8, a8, 12
    .endif
.endm
.macro RQ1                                /* requant a8 (+ bias) with q at a3, store to a4++ (a5, a6, a9-a11 scratch) */
    l32i        a9, a3, Q_MULT
    l32i        a11, a3, Q_BIAS
    ssai        31
    add         a8, a8, a11
    mull        a10, a8, a9
    mulsh       a11, a8, a9
    l32i        a9, a3, Q_RHALF
    l32i        a5, a3, Q_RSH
    add         a10, a10, a15
    saltu       a6, a10, a15
    add         a11, a11, a6
    src         a8, a11, a10
    srai        a10, a8, 31
    add         a8, a8, a9
    ssr         a5
    add         a8, a8, a10
    addi        a3, a3, Q_SIZE
    sra         a8, a8
    add         a8, a8, a12
    clamps      a8, a8, 7
    s8i         a8, a4, 0
    addi        a4, a4, 1
.endm
.macro HALF base
    LANE \base, 0
    RQ1
    LANE \base, 20
    RQ1
    LANE \base, 40
    RQ1
    LANE \base, 60
    RQ1
    LANE \base, 80
    RQ1
    LANE \base, 100
    RQ1
    LANE \base, 120
    RQ1
    LANE \base, 140
    RQ1
.endm
    .global nn_dw_rq16
    .align 4
nn_dw_rq16:
    addi        a1, a1, -16
    s32i        a12, a1, 0
    s32i        a15, a1, 4
    mov         a12, a5                   /* out_off */
    movi        a15, 0x40000000
    addi        a7, a2, 32
    HALF        a2
    HALF        a7
    l32i        a12, a1, 0
    l32i        a15, a1, 4
    addi        a1, a1, 16
    ret

/* void nn_dw_acc16_i(const int8_t *base, int ch, int row_bytes, const int8_t *f, uint8_t *raw)  - interior 3x3:
 * taps at base + y row_bytes + x ch, filter rows at f + t ch (16 channels) */
    .global nn_dw_acc16_i
    .align 4
nn_dw_acc16_i:
    ee.zero.qacc
    movi        a7, 3
1:  mov         a8, a2
    ee.vld.128.xp q0, a8, a3
    ee.vld.128.xp q1, a5, a3
    ee.vld.128.xp q2, a8, a3
    ee.vld.128.xp q3, a5, a3
    ee.vmulas.s8.qacc q0, q1
    ee.vld.128.xp q0, a8, a3
    ee.vld.128.xp q1, a5, a3
    ee.vmulas.s8.qacc q2, q3
    ee.vmulas.s8.qacc q0, q1
    add         a2, a2, a4
    addi        a7, a7, -1
    bnez        a7, 1b
    mov         a8, a6
    ee.st.qacc_l.l.128.ip a8, 16
    ee.st.qacc_l.h.32.ip a8, 16
    ee.st.qacc_h.l.128.ip a8, 16
    ee.st.qacc_h.h.32.ip a8, 0
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* int32_t nn_fc_dot_s8(const int8_t *in, const int8_t *w, int n32, const int8_t *splat16)
 * dot(in, w) + dot(splat, w) over 32 n32 bytes in one pass: the input is loaded by the fused MAC, the splat lives in
 * q7, weights alternate q1 / q2 (loaded one step ahead). in / w / splat 16-aligned; reads 16 bytes past both. */
    .global nn_fc_dot_s8
    .align 4
nn_fc_dot_s8:
    ee.zero.accx
    ee.vld.128.ip q7, a5, 0
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a3, 16
    wsr         a4, lbeg
    loopnez     a4, 1f
    ee.vld.128.ip q2, a3, 16
    ee.vmulas.s8.accx.ld.ip q0, a2, 16, q0, q1    /* in * w; next in */
    ee.vmulas.s8.accx q7, q1                      /* splat * w */
    ee.vld.128.ip q1, a3, 16
    ee.vmulas.s8.accx.ld.ip q0, a2, 16, q0, q2
    ee.vmulas.s8.accx q7, q2
1:  movi        a5, 0
    ee.srs.accx a2, a5, 0
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* void nn_relu6_v(int8_t *x, int n16) - x = min(max(x, 0), 6), 16 per step, x 16-aligned */
    .global nn_relu6_v
    .align 4
nn_relu6_v:
    addi        a1, a1, -16
    movi        a4, 6
    s8i         a4, a1, 0
    mov         a4, a1
    ee.vldbc.8.ip q7, a4, 0               /* 6 */
    ee.zero.q   q6
    addi        a1, a1, 16
    mov         a4, a2
    wsr         a3, lbeg
    loopnez     a3, 1f
    ee.vld.128.ip q0, a2, 16
    ee.vmax.s8  q0, q0, q6
    ee.vmin.s8  q0, q0, q7
    ee.vst.128.ip q0, a4, 16
1:  ret

/* void nn_vmax16(const int8_t *const *taps, int ntaps, int8_t *out, const int8_t *lohi32) - out[0..15] =
 * clamp(max_t taps[t][0..15]); lohi32 = 16 x act_min then 16 x act_max. 16-aligned. */
    .global nn_vmax16
    .align 4
nn_vmax16:
    l32i        a8, a2, 0
    addi        a2, a2, 4
    ee.vld.128.ip q0, a8, 0
    addi        a3, a3, -1
    wsr         a3, lbeg
    loopnez     a3, 1f
    l32i        a8, a2, 0
    addi        a2, a2, 4
    ee.vld.128.ip q1, a8, 0
    ee.vmax.s8  q0, q0, q1
1:  ee.vld.128.ip q2, a5, 16
    ee.vld.128.ip q3, a5, 0
    ee.vmax.s8  q0, q0, q2
    ee.vmin.s8  q0, q0, q3
    ee.vst.128.ip q0, a4, 0
    ret

/* void nn_add_lut(const int8_t *a, const int8_t *b, const int32_t *ta, const int32_t *tb, int n, const int32_t *k)
 * out[i] = clamp(rq(ta[a[i]] + tb[b[i]])) with per-tensor k = { out, mult, rsh, rhalf, negm, out_off, lo, hi };
 * ta / tb indexed by (uint8)(x + 128). */
    .global nn_add_lut
    .align 4
nn_add_lut:                               /* a2 a, a3 b, a4 ta, a5 tb, a6 n, a7 k; rsh >= 1, act [-128, 127] */
    addi        a1, a1, -16
    s32i        a12, a1, 0
    s32i        a13, a1, 4
    s32i        a14, a1, 8
    s32i        a15, a1, 12
    l32i        a12, a7, 4                /* mult */
    l32i        a13, a7, 12               /* rhalf */
    movi        a14, 0x40000000           /* nudge */
    l32i        a15, a7, 20               /* out_off */
    l32i        a8, a7, 8                 /* rsh */
    l32i        a7, a7, 0                 /* out */
    wsr         a6, lbeg
    loopnez     a6, 1f
    l8ui        a9, a2, 0                 /* tables are indexed by the raw byte */
    l8ui        a10, a3, 0
    addi        a2, a2, 1
    addi        a3, a3, 1
    addx4       a9, a9, a4
    addx4       a10, a10, a5
    l32i        a9, a9, 0
    l32i        a10, a10, 0
    ssai        31
    add         a9, a9, a10               /* ta[a] + tb[b] */
    mull        a10, a9, a12
    mulsh       a11, a9, a12
    add         a10, a10, a14
    saltu       a6, a10, a14
    add         a11, a11, a6
    src         a9, a11, a10
    srai        a10, a9, 31
    add         a9, a9, a13
    add         a9, a9, a10
    ssr         a8
    sra         a9, a9
    add         a9, a9, a15
    clamps      a9, a9, 7
    s8i         a9, a7, 0
    addi        a7, a7, 1
1:  l32i        a12, a1, 0
    l32i        a13, a1, 4
    l32i        a14, a1, 8
    l32i        a15, a1, 12
    addi        a1, a1, 16
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* void nn_avg16(const uint8_t *raw, int8_t *out, int k, const int32_t *lohi) - 16 QACC lane sums (nn_dw_acc16 with a
 * ones filter) -> TFLite rounded average by 2^k (ties away from zero, truncation toward zero), clamp, store */
.macro AVG1 base, bit
    LANE \base, \bit
    srai        a10, a8, 31               /* sign */
    xor         a8, a8, a10
    sub         a8, a8, a10               /* |s| */
    add         a8, a8, a6                /* + 2^(k-1) */
    ssr         a4
    srl         a8, a8                    /* >> k */
    xor         a8, a8, a10
    sub         a8, a8, a10               /* sign back */
    max         a8, a8, a11
    min         a8, a8, a7
    s8i         a8, a3, 0
    addi        a3, a3, 1
.endm
.macro AVGHALF base
    AVG1 \base, 0
    AVG1 \base, 20
    AVG1 \base, 40
    AVG1 \base, 60
    AVG1 \base, 80
    AVG1 \base, 100
    AVG1 \base, 120
    AVG1 \base, 140
.endm
    .global nn_avg16
    .align 4
nn_avg16:
    movi        a6, 1
    ssl         a4
    sll         a6, a6
    srli        a6, a6, 1                 /* 2^(k-1) (0 for k = 0) */
    l32i        a11, a5, 0                /* act_min */
    l32i        a7, a5, 4                 /* act_max */
    addi        a5, a2, 32
    AVGHALF     a2
    AVGHALF     a5
    ret
#endif
#if SOC_CPU_HAS_PIE
    .section .iram.text, "ax"
/* void nn_avg4_16(const int8_t *p0, const int8_t *p1, const int8_t *p2, const int8_t *p3, int8_t *out, const int8_t *cfg)
 * 16 channels of a 2x2 average: widen to int16 (zip with the sign mask), sum, y = (s + 2 - [s < 0]) >> 2 (= TFLite's
 * rounded average), pack, clamp. cfg: act_min x16, act_max x16, int16 2 x8, int16 1 x8. All 16-aligned. */
.macro WIDEN_ADD qx, qhi
    ee.vcmp.lt.s8 \qhi, \qx, q7
    ee.vzip.8   \qx, \qhi
.endm
    .global nn_avg4_16
    .align 4
nn_avg4_16:
    movi        a8, 2
    wsr         a8, sar
    ee.zero.q   q7
    ee.vld.128.ip q0, a2, 0
    ee.vld.128.ip q1, a3, 0
    WIDEN_ADD   q0, q4                    /* q0 lo, q4 hi */
    WIDEN_ADD   q1, q5
    ee.vadds.s16 q0, q0, q1
    ee.vadds.s16 q4, q4, q5
    ee.vld.128.ip q1, a4, 0
    WIDEN_ADD   q1, q5
    ee.vadds.s16 q0, q0, q1
    ee.vadds.s16 q4, q4, q5
    ee.vld.128.ip q1, a5, 0
    WIDEN_ADD   q1, q5
    ee.vadds.s16 q0, q0, q1
    ee.vadds.s16 q4, q4, q5
    addi        a8, a7, 32
    ee.vld.128.ip q2, a8, 16              /* 2 x8 */
    ee.vld.128.ip q3, a8, 0               /* 1 x8 */
    ee.vcmp.lt.s16 q5, q0, q7             /* -1 where negative */
    ee.vadds.s16 q0, q0, q2
    ee.vadds.s16 q0, q0, q5
    ee.vcmp.lt.s16 q5, q4, q7
    ee.vadds.s16 q4, q4, q2
    ee.vadds.s16 q4, q4, q5
    ee.vmul.s16 q0, q0, q3                /* >> 2 (SAR) */
    ee.vmul.s16 q4, q4, q3
    ee.vunzip.8 q0, q4                    /* low bytes of the 16 lanes */
    ee.vld.128.ip q1, a7, 16
    ee.vld.128.ip q2, a7, 0
    ee.vmax.s8  q0, q0, q1
    ee.vmin.s8  q0, q0, q2
    ee.vst.128.ip q0, a6, 0
    ret

/* void nn_max4_16(p0, p1, p2, p3, out, cfg) - 16 channels of a 2x2 max, clamp */
    .global nn_max4_16
    .align 4
nn_max4_16:
    ee.vld.128.ip q0, a2, 0
    ee.vld.128.ip q1, a3, 0
    ee.vld.128.ip q2, a4, 0
    ee.vld.128.ip q3, a5, 0
    ee.vmax.s8  q0, q0, q1
    ee.vmax.s8  q2, q2, q3
    ee.vld.128.ip q4, a7, 16
    ee.vld.128.ip q5, a7, 0
    ee.vmax.s8  q0, q0, q2
    ee.vmax.s8  q0, q0, q4
    ee.vmin.s8  q0, q0, q5
    ee.vst.128.ip q0, a6, 0
    ret
#endif
