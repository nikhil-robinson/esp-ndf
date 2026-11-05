/* ESP32-P4 vector kernels (Xespv2p2 "PIE", TRM chapter 4): the integer part of the dsp.h API, same contracts as
 * components/lib/dsp.S on the S3. RISC-V ABI: args a0..a7, result a0, a0-a7 / t0-t6 scratch. The vector state
 * (q0-q7, XACC) is not saved by interrupt handlers: no handler may use it. Loads and stores run in the reset
 * "force alignment" mode: pointers must be 16-byte aligned, as the dsp.h contracts say. The assembler only takes
 * a0-a5, s0, s1, t3-t6, s8-s11 as register operands of esp.* instructions (rs1 field is bit 4 + bits 2:0).
 * Checked on the host with tools/p4_pie (instruction-level model of the TRM, same ELF); NOT run on silicon. */
    .option arch, +xespv
    .text

/* int32 dsp_dot_s16(const int16_t *a, const int16_t *b, int n)   n multiple of 16 (here: of 8). 40-bit XACC, result
 * saturated to 32 bits. */
    .global dsp_dot_s16
    .align 2
dsp_dot_s16:
    esp.zero.xacc
    srli        a3, a2, 3
    beqz        a3, 2f
1:  esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a1, 16
    esp.vmulas.s16.xacc q0, q1
    addi        a3, a3, -1
    bnez        a3, 1b
2:  esp.movx.r.xacc.l a0
    esp.movx.r.xacc.h a3
    slli        a3, a3, 24
    srai        a3, a3, 24                /* XACC[39:32] signed */
    srai        a4, a0, 31
    beq         a3, a4, 3f                /* fits in 32 bits */
    srai        a3, a3, 31                /* -1 / 0 */
    lui         a0, 0x80000
    addi        a0, a0, -1                /* 0x7fffffff */
    xor         a0, a0, a3                /* 0x80000000 when negative */
3:  ret

/* void dsp_dot2_s16(const int16_t *a, const int16_t *b, const int16_t *h, int n, int32_t out[2]) */
    .global dsp_dot2_s16
    .align 2
dsp_dot2_s16:
    addi        sp, sp, -32
    sw          ra, 28(sp)
    sw          s0, 24(sp)
    sw          s1, 20(sp)
    sw          s2, 16(sp)
    sw          s3, 12(sp)
    mv          s0, a1
    mv          s1, a2
    mv          s2, a3
    mv          s3, a4
    mv          a1, a2                    /* a . h */
    mv          a2, a3
    call        dsp_dot_s16
    sw          a0, 0(s3)
    mv          a0, s0                    /* b . h */
    mv          a1, s1
    mv          a2, s2
    call        dsp_dot_s16
    sw          a0, 4(s3)
    lw          ra, 28(sp)
    lw          s0, 24(sp)
    lw          s1, 20(sp)
    lw          s2, 16(sp)
    lw          s3, 12(sp)
    addi        sp, sp, 32
    ret

/* void dsp_add_s16_sat(int16_t *dst, const int16_t *a, const int16_t *b, int n)   n multiple of 16 (here: of 8) */
    .global dsp_add_s16_sat
    .align 2
dsp_add_s16_sat:
    srli        a3, a3, 3
    beqz        a3, 2f
1:  esp.vld.128.ip q0, a1, 16
    esp.vld.128.ip q1, a2, 16
    esp.vadd.s16 q2, q0, q1
    esp.vst.128.ip q2, a0, 16
    addi        a3, a3, -1
    bnez        a3, 1b
2:  ret

/* void dsp_copy128(void *dst, const void *src, size_t n)   n multiple of 32 */
    .global dsp_copy128
    .align 2
dsp_copy128:
    srli        a3, a2, 5
    beqz        a3, 2f
1:  esp.vld.128.ip q0, a1, 16
    esp.vld.128.ip q1, a1, 16
    esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q1, a0, 16
    addi        a3, a3, -1
    bnez        a3, 1b
2:  ret

/* void dsp_set128(void *dst, const void *pattern16, size_t n)   n multiple of 32 */
    .global dsp_set128
    .align 2
dsp_set128:
    esp.vld.128.ip q0, a1, 0
    srli        a3, a2, 5
    beqz        a3, 2f
1:  esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q0, a0, 16
    addi        a3, a3, -1
    bnez        a3, 1b
2:  ret

/* void dsp_copy128u(void *dst, const void *src, size_t n)   dst 16-byte aligned, src any alignment, n multiple of 48.
 * First load sets SAR_BYTE, ESP.SRC.Q joins every two neighbouring aligned blocks. Reads up to 32 bytes past src + n. */
    .global dsp_copy128u
    .align 2
dsp_copy128u:
    srli        a3, a2, 4
    beqz        a3, 2f
    esp.ld.128.usar.ip q0, a1, 16
1:  esp.vld.128.ip q1, a1, 16
    esp.src.q   q2, q0, q1
    esp.vst.128.ip q2, a0, 16
    esp.orq     q0, q1, q1
    addi        a3, a3, -1
    bnez        a3, 1b
2:  ret

/* int32_t dsp_dotu_s16(const int16_t *x, const int16_t *h, int groups8)   sum x[i] h[i] over 8 groups8 values;
 * x only 2-byte aligned (ESP.LD.128.USAR.IP + ESP.SRC.Q), h 16-aligned; XACC, result saturated to 32 bits.
 * Reads up to 16 bytes past the end of x. */
    .global dsp_dotu_s16
    .align 2
dsp_dotu_s16:
    esp.zero.xacc
    esp.ld.128.usar.ip q0, a0, 16
    beqz        a2, 2f
1:  esp.ld.128.usar.ip q1, a0, 16
    esp.vld.128.ip q3, a1, 16
    esp.src.q   q2, q0, q1
    esp.orq     q0, q1, q1
    esp.vmulas.s16.xacc q2, q3
    addi        a2, a2, -1
    bnez        a2, 1b
2:  li          t3, 0
    esp.srs.s.xacc a0, t3
    ret

/* void dsp_fird_core_s16(const int16_t *x0, const int16_t *hr, int16_t *out, int n_out, int step_bytes, int groups8,
 *                        int shift, int rnd)   out[i] = clamp16((dot(x0 + i step, hr) + rnd) >> shift) */
    .global dsp_fird_core_s16
    .align 2
dsp_fird_core_s16:
    beqz        a3, 9f
.Lfo:
    mv          t3, a0
    mv          t4, a1
    mv          t6, a5
    esp.zero.xacc
    esp.ld.128.usar.ip q0, t3, 16
    beqz        t6, 2f
1:  esp.ld.128.usar.ip q1, t3, 16
    esp.vld.128.ip q3, t4, 16
    esp.src.q   q2, q0, q1
    esp.orq     q0, q1, q1
    esp.vmulas.s16.xacc q2, q3
    addi        t6, t6, -1
    bnez        t6, 1b
2:  li          t5, 0
    esp.srs.s.xacc t6, t5
    add         t6, t6, a7
    sra         t6, t6, a6
    li          t0, 32767
    ble         t6, t0, 3f
    mv          t6, t0
3:  li          t0, -32768
    bge         t6, t0, 4f
    mv          t6, t0
4:  sh          t6, 0(a2)
    addi        a2, a2, 2
    add         a0, a0, a4
    addi        a3, a3, -1
    bnez        a3, .Lfo
9:  ret

/* void dsp_vadds16_sh(const int16_t *a, const int16_t *b, int16_t *y, int blocks8, int shift)
 * y = sat16(sat16(a + b) >> shift), 8 values per block, all 16-aligned. The shift is x * 1 >> SAR (ESP.VMUL.S16). */
    .global dsp_vadds16_sh
    .align 2
dsp_vadds16_sh:
    beqz        a3, 9f
    bnez        a4, 3f
1:  esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a1, 16
    esp.vadd.s16 q2, q0, q1
    esp.vst.128.ip q2, a2, 16
    addi        a3, a3, -1
    bnez        a3, 1b
    ret
3:  esp.movx.w.sar a4
    addi        sp, sp, -16
    li          t0, 1
    sh          t0, 0(sp)
    mv          a5, sp
    esp.vldbc.16.ip q7, a5, 0             /* ones */
    addi        sp, sp, 16
4:  esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a1, 16
    esp.vadd.s16 q2, q0, q1
    esp.vmul.s16 q2, q2, q7
    esp.vst.128.ip q2, a2, 16
    addi        a3, a3, -1
    bnez        a3, 4b
9:  ret

/* void dsp_vmuls16_sh(const int16_t *a, const int16_t *b, int16_t *y, int blocks16, int shift)
 * y = sat16((a * b) >> shift): product into the 64-bit QACC lanes, ESP.SRCMB.S16.QACC shifts and saturates. */
    .global dsp_vmuls16_sh
    .align 2
dsp_vmuls16_sh:
    slli        a3, a3, 1                 /* 8 values per step */
    beqz        a3, 9f
1:  esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a1, 16
    esp.zero.qacc
    esp.vmulas.s16.qacc q0, q1
    esp.srcmb.s16.qacc q2, a4, 1
    esp.vst.128.ip q2, a2, 16
    addi        a3, a3, -1
    bnez        a3, 1b
9:  ret

/* void dsp_mm8_s16(const int16_t *A, const int16_t *B, int16_t *C, int m, int n, int k, int shift)
 * C[i][j..j+7] = sat16(sum_p A[i][p] B[p][j..j+7] >> shift): A[i][p] broadcast (ESP.VLDBC.16), 8 lanes of QACC
 * (64 bit each) accumulate, ESP.SRCMB.S16.QACC shifts and saturates. k % 8 == 0, B / C 16-aligned. */
    .global dsp_mm8_s16
    .align 2
dsp_mm8_s16:
    slli        a5, a5, 1                 /* B / C row stride (bytes) */
    slli        t1, a4, 1                 /* A row stride */
    mv          t6, a6                    /* shift (PIE operand registers: a0-a5, s0, s1, t3-t6, s8-s11) */
.Lmr:
    li          t2, 0                     /* column byte offset */
.Lmc:
    mv          t3, a0
    add         t4, a1, t2
    mv          t0, a4
    esp.zero.qacc
1:  esp.vldbc.16.ip q1, t3, 2
    esp.vld.128.xp q0, t4, a5
    esp.vmulas.s16.qacc q0, q1
    addi        t0, t0, -1
    bnez        t0, 1b
    esp.srcmb.s16.qacc q3, t6, 1
    esp.vst.128.ip q3, a2, 16             /* C advances one column block; after the row it is at the next row */
    addi        t2, t2, 16
    blt         t2, a5, .Lmc
    add         a0, a0, t1
    addi        a3, a3, -1
    bnez        a3, .Lmr
    ret

/* void dsp_fft_s16_stage(int16_t *x, int n2, int blocks, const int16_t *wbr) - one radix-2 stage of dsp_fft_s16
 * (natural-order input, bit-reversed twiddles as ESP-DSP dsps_fft2r_sc16): for every block j, w = wbr[j] (cos, sin
 * Q15) broadcast; top' = (top + conj(w) bot) / 2, bot' = (top - conj(w) bot) / 2, four complex per vector step.
 * ESP.CMUL.S16 selectors 2 and 3 are conj(qx) * qy for complex 0-1 and 2-3, SAR = 16 halves the product; the top half
 * comes from x * 32767 >> 16. n2 % 4 == 0, x 16-aligned. */
    .global dsp_fft_s16_stage
    .align 2
dsp_fft_s16_stage:
    addi        sp, sp, -16
    li          t0, 32767
    sh          t0, 0(sp)
    mv          a4, sp
    esp.vldbc.16.ip q6, a4, 0
    addi        sp, sp, 16
    li          t3, 16
    esp.movx.w.sar t3
    srli        t0, a1, 2                 /* vector steps per block */
    slli        t1, a1, 2                 /* n2 complex = 4 n2 bytes */
    mv          a5, a0                    /* top */
1:  add         a4, a5, t1                /* bottom */
    esp.vldbc.32.ip q5, a3, 4             /* w of this block */
    mv          t2, t0
2:  esp.vld.128.ip q0, a4, 0
    esp.vld.128.ip q2, a5, 0
    esp.cmul.s16 q1, q5, q0, 2
    esp.vmul.s16 q2, q2, q6
    esp.cmul.s16 q1, q5, q0, 3
    esp.vsub.s16 q3, q2, q1
    esp.vadd.s16 q4, q2, q1
    esp.vst.128.ip q3, a4, 16
    esp.vst.128.ip q4, a5, 16
    addi        t2, t2, -1
    bnez        t2, 2b
    add         a5, a5, t1                /* next block: skip its bottom half */
    addi        a2, a2, -1
    bnez        a2, 1b
    ret
