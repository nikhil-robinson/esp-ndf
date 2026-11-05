/* ESP32-P4 vector kernels for components/lib/nn.c (Xespv2p2 "PIE", TRM chapter 4): the same symbols and contracts as
 * nn_s8.S on the S3, written for the P4 instruction set. 8-bit MAC into XACC (40 bit) or the 16 x 32-bit QACC lanes
 * (the S3 has 20-bit lanes: the 64-byte "raw" buffer of nn_dw_acc16 / nn_dw_rq16 / nn_avg16 holds 16 int32 here).
 * Requantisation is TFLite's: r = floor((acc * mult + 2^30) / 2^31); y = (r + rhalf + (r < 0 ? negm : 0)) >> rsh.
 * Registers the assembler takes as esp.* operands: a0-a5, s0, s1, t3-t6, s8-s11. No interrupt handler may use the
 * vector unit (nothing saves it). Checked on the host with tools/p4_pie; NOT run on silicon. */
    .option arch, +xespv
    .text

#define Q_BIAS   0
#define Q_MULT   4
#define Q_RSH    8
#define Q_RHALF  12
#define Q_NEGM   16
#define Q_SIZE   20

/* value \v (+ bias, requantised) with the per-channel entry at \q; scratch t0 t1 t2 a6, nudge 2^30 in s5 */
.macro RQCORE v, q
    lw          t0, Q_BIAS(\q)
    add         \v, \v, t0
    lw          t0, Q_MULT(\q)
    mul         t1, \v, t0
    mulh        t2, \v, t0
    add         t1, t1, s5
    sltu        a6, t1, s5                /* carry of the +2^30 */
    add         t2, t2, a6
    slli        t2, t2, 1
    srli        t1, t1, 31
    or          \v, t2, t1                /* (hi:lo) >> 31 */
    lw          t0, Q_NEGM(\q)
    srai        t1, \v, 31
    and         t1, t1, t0
    lw          t0, Q_RHALF(\q)
    add         \v, \v, t1
    add         \v, \v, t0
    lw          t0, Q_RSH(\q)
    sra         \v, \v, t0
.endm

.macro CLAMP v, lo, hi                    /* lo, hi: registers */
    bge         \v, \lo, .Lcl\@
    mv          \v, \lo
.Lcl\@:
    ble         \v, \hi, .Lch\@
    mv          \v, \hi
.Lch\@:
.endm

.macro SAVE4
    addi        sp, sp, -16
    sw          s2, 0(sp)
    sw          s3, 4(sp)
    sw          s4, 8(sp)
    sw          s5, 12(sp)
.endm
.macro REST4
    lw          s2, 0(sp)
    lw          s3, 4(sp)
    lw          s4, 8(sp)
    lw          s5, 12(sp)
    addi        sp, sp, 16
.endm

/* int32_t nn_dot_s8(const int8_t *a, const int8_t *b, int n16)   sum a[i] b[i] over 16 n16 bytes; a any alignment
 * (ESP.LD.128.USAR.IP + ESP.SRC.Q), b 16-aligned; result saturated to 32 bits. Reads up to 16 bytes past a. */
    .global nn_dot_s8
    .align 2
nn_dot_s8:
    esp.zero.xacc
    esp.ld.128.usar.ip q0, a0, 16
    beqz        a2, 2f
1:  esp.ld.128.usar.ip q1, a0, 16
    esp.vld.128.ip q3, a1, 16
    esp.src.q   q2, q0, q1
    esp.orq     q0, q1, q1
    esp.vmulas.s8.xacc q2, q3
    addi        a2, a2, -1
    bnez        a2, 1b
2:  li          t3, 0
    esp.srs.s.xacc a0, t3
    ret

/* void nn_conv_row_s8(const int8_t *row, int k16, const int8_t *w, int oc, const nn_chq_t *q, int8_t *out)
 * For every output channel o: acc = dot(row[0..16 k16), w[o]) + q[o].bias, requantised, + out_off, clamped to
 * [act_min, act_max], stored. Constants out_off, act_min, act_max at q[-20], q[-16], q[-12]. row and w 16-aligned. */
    .global nn_conv_row_s8
    .align 2
nn_conv_row_s8:
    SAVE4
    lw          s2, -20(a4)
    lw          s3, -16(a4)
    lw          s4, -12(a4)
    li          s5, 0x40000000
1:  mv          t3, a0
    mv          t6, a1
    esp.zero.xacc
2:  esp.vld.128.ip q0, t3, 16
    esp.vld.128.ip q1, a2, 16
    esp.vmulas.s8.xacc q0, q1
    addi        t6, t6, -1
    bnez        t6, 2b
    li          t4, 0
    esp.srs.s.xacc t5, t4
    RQCORE      t5, a4
    add         t5, t5, s2
    CLAMP       t5, s3, s4
    sb          t5, 0(a5)
    addi        a5, a5, 1
    addi        a4, a4, Q_SIZE
    addi        a3, a3, -1
    bnez        a3, 1b
    REST4
    ret

/* void nn_dw_acc16(const int8_t *const *taps, int ntaps, const int8_t *f, int fstride, uint8_t *raw)
 * QACC (16 x int32 lanes) = sum_t taps[t][0..15] * f[t * fstride + 0..15]; raw <- the 16 lanes (64 bytes).
 * taps / f 16-aligned. */
    .global nn_dw_acc16
    .align 2
nn_dw_acc16:
    esp.zero.qacc
    beqz        a1, 2f
1:  lw          t3, 0(a0)
    addi        a0, a0, 4
    esp.vld.128.ip q0, t3, 0
    esp.vld.128.xp q1, a2, a3
    esp.vmulas.s8.qacc q0, q1
    addi        a1, a1, -1
    bnez        a1, 1b
2:  esp.st.qacc.l.l.128.ip a4, 16
    esp.st.qacc.l.h.128.ip a4, 16
    esp.st.qacc.h.l.128.ip a4, 16
    esp.st.qacc.h.h.128.ip a4, 0
    ret

/* void nn_dw_acc16_i(const int8_t *base, int ch, int row_bytes, const int8_t *f, uint8_t *raw) - interior 3x3:
 * taps at base + y row_bytes + x ch, filter rows at f + t ch (16 channels) */
    .global nn_dw_acc16_i
    .align 2
nn_dw_acc16_i:
    esp.zero.qacc
    li          t0, 3
1:  mv          t3, a0
    esp.vld.128.xp q0, t3, a1
    esp.vld.128.xp q1, a3, a1
    esp.vmulas.s8.qacc q0, q1
    esp.vld.128.xp q0, t3, a1
    esp.vld.128.xp q1, a3, a1
    esp.vmulas.s8.qacc q0, q1
    esp.vld.128.xp q0, t3, a1
    esp.vld.128.xp q1, a3, a1
    esp.vmulas.s8.qacc q0, q1
    add         a0, a0, a2
    addi        t0, t0, -1
    bnez        t0, 1b
    esp.st.qacc.l.l.128.ip a4, 16
    esp.st.qacc.l.h.128.ip a4, 16
    esp.st.qacc.h.l.128.ip a4, 16
    esp.st.qacc.h.h.128.ip a4, 0
    ret

/* void nn_dw_rq16(const uint8_t *raw, const nn_chq_t *q, int8_t *out, int32_t out_off)
 * raw = 16 int32 lanes (nn_dw_acc16); lane i + q[i].bias -> requant -> + out_off -> clamp [-128, 127] -> out[i]. */
    .global nn_dw_rq16
    .align 2
nn_dw_rq16:
    SAVE4
    li          s5, 0x40000000
    li          s3, -128
    li          s4, 127
    li          s2, 16
1:  lw          t5, 0(a0)
    addi        a0, a0, 4
    RQCORE      t5, a1
    add         t5, t5, a3
    CLAMP       t5, s3, s4
    sb          t5, 0(a2)
    addi        a2, a2, 1
    addi        a1, a1, Q_SIZE
    addi        s2, s2, -1
    bnez        s2, 1b
    REST4
    ret

/* void nn_rq_s8(const int32_t *acc, int n, const nn_chq_t *q, int8_t *out, int32_t out_off) - acc[i] + q[i].bias,
 * requantised, + out_off, clamp [-128, 127], out[i] */
    .global nn_rq_s8
    .align 2
nn_rq_s8:
    beqz        a1, 9f
    SAVE4
    li          s5, 0x40000000
    li          s3, -128
    li          s4, 127
1:  lw          t5, 0(a0)
    addi        a0, a0, 4
    RQCORE      t5, a2
    add         t5, t5, a4
    CLAMP       t5, s3, s4
    sb          t5, 0(a3)
    addi        a3, a3, 1
    addi        a2, a2, Q_SIZE
    addi        a1, a1, -1
    bnez        a1, 1b
    REST4
9:  ret

/* int32_t nn_fc_dot_s8(const int8_t *in, const int8_t *w, int n32, const int8_t *splat16)
 * dot(in, w) + dot(splat, w) over 32 n32 bytes in one pass (splat: 16 copies of one byte). All 16-aligned. */
    .global nn_fc_dot_s8
    .align 2
nn_fc_dot_s8:
    esp.zero.xacc
    esp.vld.128.ip q7, a3, 0
    beqz        a2, 2f
1:  esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a1, 16
    esp.vld.128.ip q2, a0, 16
    esp.vld.128.ip q3, a1, 16
    esp.vmulas.s8.xacc q0, q1
    esp.vmulas.s8.xacc q7, q1
    esp.vmulas.s8.xacc q2, q3
    esp.vmulas.s8.xacc q7, q3
    addi        a2, a2, -1
    bnez        a2, 1b
2:  li          t3, 0
    esp.srs.s.xacc a0, t3
    ret

/* void nn_relu6_v(int8_t *x, int n16) - x = min(max(x, 0), 6), 16 per step, x 16-aligned */
    .global nn_relu6_v
    .align 2
nn_relu6_v:
    beqz        a1, 9f
    addi        sp, sp, -16
    li          t0, 6
    sb          t0, 0(sp)
    mv          a2, sp
    esp.vldbc.8.ip q7, a2, 0              /* 6 */
    addi        sp, sp, 16
    esp.xorq    q6, q6, q6
1:  esp.vld.128.ip q0, a0, 0
    esp.vmax.s8 q0, q0, q6
    esp.vmin.s8 q0, q0, q7
    esp.vst.128.ip q0, a0, 16
    addi        a1, a1, -1
    bnez        a1, 1b
9:  ret

/* void nn_vmax16(const int8_t *const *taps, int ntaps, int8_t *out, const int8_t *lohi32) - out[0..15] =
 * clamp(max_t taps[t][0..15]); lohi32 = 16 x act_min then 16 x act_max. 16-aligned. */
    .global nn_vmax16
    .align 2
nn_vmax16:
    lw          t3, 0(a0)
    addi        a0, a0, 4
    esp.vld.128.ip q0, t3, 0
    addi        a1, a1, -1
    beqz        a1, 2f
1:  lw          t3, 0(a0)
    addi        a0, a0, 4
    esp.vld.128.ip q1, t3, 0
    esp.vmax.s8 q0, q0, q1
    addi        a1, a1, -1
    bnez        a1, 1b
2:  esp.vld.128.ip q2, a3, 16
    esp.vld.128.ip q3, a3, 0
    esp.vmax.s8 q0, q0, q2
    esp.vmin.s8 q0, q0, q3
    esp.vst.128.ip q0, a2, 0
    ret

/* void nn_max4_16(p0, p1, p2, p3, out, cfg) - 16 channels of a 2x2 max, clamped to cfg (act_min x16, act_max x16) */
    .global nn_max4_16
    .align 2
nn_max4_16:
    esp.vld.128.ip q0, a0, 0
    esp.vld.128.ip q1, a1, 0
    esp.vld.128.ip q2, a2, 0
    esp.vld.128.ip q3, a3, 0
    esp.vmax.s8 q0, q0, q1
    esp.vmax.s8 q2, q2, q3
    esp.vld.128.ip q4, a5, 16
    esp.vld.128.ip q5, a5, 0
    esp.vmax.s8 q0, q0, q2
    esp.vmax.s8 q0, q0, q4
    esp.vmin.s8 q0, q0, q5
    esp.vst.128.ip q0, a4, 0
    ret

/* void nn_avg4_16(p0, p1, p2, p3, out, cfg) - 16 channels of a 2x2 average: the four taps summed in the QACC lanes
 * (times a vector of ones), y = (s + 2 - [s < 0]) >> 2 (TFLite's rounded average), clamp to cfg[0] / cfg[16]. */
    .global nn_avg4_16
    .align 2
nn_avg4_16:
    addi        sp, sp, -80
    li          t0, 1
    sb          t0, 64(sp)
    addi        t3, sp, 64
    esp.vldbc.8.ip q7, t3, 0              /* ones */
    esp.zero.qacc
    esp.vld.128.ip q0, a0, 0
    esp.vld.128.ip q1, a1, 0
    esp.vld.128.ip q2, a2, 0
    esp.vld.128.ip q3, a3, 0
    esp.vmulas.s8.qacc q0, q7
    esp.vmulas.s8.qacc q1, q7
    esp.vmulas.s8.qacc q2, q7
    esp.vmulas.s8.qacc q3, q7
    mv          t3, sp
    esp.st.qacc.l.l.128.ip t3, 16
    esp.st.qacc.l.h.128.ip t3, 16
    esp.st.qacc.h.l.128.ip t3, 16
    esp.st.qacc.h.h.128.ip t3, 0
    lb          t1, 0(a5)                 /* act_min */
    lb          t2, 16(a5)                /* act_max */
    mv          t3, sp
    li          a6, 16
1:  lw          t0, 0(t3)
    addi        t3, t3, 4
    srai        a7, t0, 31                /* -1 if negative */
    add         t0, t0, a7
    addi        t0, t0, 2
    srai        t0, t0, 2
    CLAMP       t0, t1, t2
    sb          t0, 0(a4)
    addi        a4, a4, 1
    addi        a6, a6, -1
    bnez        a6, 1b
    addi        sp, sp, 80
    ret

/* void nn_avg16(const uint8_t *raw, int8_t *out, int k, const int32_t *lohi) - 16 lane sums (nn_dw_acc16 with a ones
 * filter) -> rounded average by 2^k (ties away from zero, truncation toward zero), clamp lohi[0] .. lohi[1] */
    .global nn_avg16
    .align 2
nn_avg16:
    li          t0, 1
    sll         t0, t0, a2
    srli        t0, t0, 1                 /* 2^(k-1) (0 for k = 0) */
    lw          t1, 0(a3)
    lw          t2, 4(a3)
    li          a6, 16
1:  lw          a4, 0(a0)
    addi        a0, a0, 4
    srai        a5, a4, 31                /* sign */
    xor         a4, a4, a5
    sub         a4, a4, a5                /* |s| */
    add         a4, a4, t0
    srl         a4, a4, a2
    xor         a4, a4, a5
    sub         a4, a4, a5
    CLAMP       a4, t1, t2
    sb          a4, 0(a1)
    addi        a1, a1, 1
    addi        a6, a6, -1
    bnez        a6, 1b
    ret

/* void nn_add_lut(const int8_t *a, const int8_t *b, const int32_t *ta, const int32_t *tb, int n, const int32_t *k)
 * out[i] = clamp[-128, 127](rq(ta[a[i]] + tb[b[i]])) with k = { out, mult, rsh, rhalf, negm, out_off, lo, hi };
 * ta / tb indexed by the raw byte. Plain RISC-V: the table lookups are the work. */
    .global nn_add_lut
    .align 2
nn_add_lut:
    beqz        a4, 9f
    lw          a6, 4(a5)                 /* mult */
    lw          a7, 12(a5)                /* rhalf */
    lw          t4, 16(a5)                /* negm */
    lw          t5, 20(a5)                /* out_off */
    lw          t6, 8(a5)                 /* rsh */
    lw          t3, 0(a5)                 /* out */
    li          s0, 0x40000000
    addi        sp, sp, -16
    sw          s0, 0(sp)
    sw          s1, 4(sp)
    li          s1, 127
1:  lbu         t0, 0(a0)
    lbu         t1, 0(a1)
    addi        a0, a0, 1
    addi        a1, a1, 1
    slli        t0, t0, 2
    slli        t1, t1, 2
    add         t0, t0, a2
    add         t1, t1, a3
    lw          t0, 0(t0)
    lw          t1, 0(t1)
    add         t0, t0, t1                /* ta[a] + tb[b] */
    mul         t1, t0, a6
    mulh        t2, t0, a6
    lw          t0, 0(sp)
    add         t1, t1, t0
    sltu        t0, t1, t0
    add         t2, t2, t0
    slli        t2, t2, 1
    srli        t1, t1, 31
    or          t0, t2, t1                /* r */
    srai        t1, t0, 31
    and         t1, t1, t4
    add         t0, t0, t1
    add         t0, t0, a7
    sra         t0, t0, t6
    add         t0, t0, t5
    li          t1, -128
    bge         t0, t1, 2f
    mv          t0, t1
2:  ble         t0, s1, 3f
    mv          t0, s1
3:  sb          t0, 0(t3)
    addi        t3, t3, 1
    addi        a4, a4, -1
    bnez        a4, 1b
    lw          s0, 0(sp)
    lw          s1, 4(sp)
    addi        sp, sp, 16
9:  ret
