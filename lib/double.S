/* Double add/sub/mul fast paths, Xtensa CALL0. Normal operands take the
 * fast code; zero, inf, NaN and denormal operands (and a multiply whose
 * result leaves the normal range) tail-jump to the general C code in
 * lib/double.c with the operands untouched. Round to nearest even, bit-exact (libc_test).
 * Arguments: x = a3:a2 (hi:lo), y = a5:a4; result a3:a2. */
#ifdef __XTENSA__
    .text
    .literal_position

/* ---- add / sub ------------------------------------------------------------
 * Both operands normal (else the C code). x and y are kept packed: y's
 * mantissa, aligned to x's exponent, is added to / subtracted from x's packed
 * bits directly. If the exponent field did not move, only rounding is left;
 * if it moved by one binade, one shift and an exponent fix-up repair it.
 * Shifted-out bits of y live in a guard word g (a9): bit 31 = half, lower
 * bits sticky. Exponent gaps of 0 or 1 with different signs are exact
 * (cancellation) and handled separately. Every case is written for "X has
 * the larger exponent" once (macros) and instantiated for both orders, so
 * nothing is swapped. Branches use 12-bit-range forms where targets are far:
 * the 8-bit compare branches would be relaxed into branch + jump.
 * a7 = 0x7FF00000 throughout. */

/* Return XH:XL (moved to a3:a2 when the roles are swapped). */
.macro EXIT XL, XH
.ifnc \XL, a2
    mov     a2, \XL
    mov     a3, \XH
.endif
    ret
.endm

/* Round packed XH:XL on g and return: ties to even by "add one, clear bit 0".
 * A carry into the exponent is right (next binade, or inf). */
.macro ROUND_G XL, XH
    bltz    a9, .Lrg0\@
    EXIT    \XL, \XH
.Lrg0\@:
    addi    \XL, \XL, 1
    bnez    \XL, .Lrg1\@
    addi    \XH, \XH, 1
.Lrg1\@:
    slli    a9, a9, 1
    bnez    a9, .Lrg2\@
    srli    \XL, \XL, 1
    slli    \XL, \XL, 1
.Lrg2\@:
    EXIT    \XL, \XH
.endm

/* Y mantissa (implicit bit set) shifted right by d (a8, < 64) with g.
 * d < 32 falls through; d >= 32 goes to ALIGN_Y_BIG, emitted out of line. */
.macro ALIGN_Y YL, YH, big, back
    or      \YH, \YH, a7             /* exponent bits all 1: the lowest becomes the implicit bit */
    slli    \YH, \YH, 11
    srli    \YH, \YH, 11
    bgeui   a8, 32, \big
    ssr     a8                       /* d < 32 (d = 0: nothing moves, g = 0) */
    sll     a9, \YL                  /* g: bits shifted out, top aligned */
    src     \YL, \YH, \YL
    srl     \YH, \YH
\back:
.endm
.macro ALIGN_Y_BIG YL, YH, big, back
\big:
    addi    a8, a8, -32
    ssr     a8
    sll     a10, \YL                 /* bits below g */
    src     a9, \YH, \YL
    srl     \YL, \YH
    movi    \YH, 0
    beqz    a10, \back
    movi    a10, 1
    or      a9, a9, a10              /* fold into g's sticky bit */
    j       \back
.endm

/* Same signs. a6 = X sign+exponent (12 bits), a8 = d >= 0. */
.macro SAME_SIGN XL, XH, YL, YH
    srli    a10, a8, 6
    bnez    a10, .Lsx\@              /* d >= 64: |Y| below half an ulp of X */
    ALIGN_Y \YL, \YH, .Lsb\@, .Lsa\@
    add     \XL, \XL, \YL
    add     \XH, \XH, \YH
    bgeu    \XL, \YL, .Lsc\@
    addi    \XH, \XH, 1
.Lsc\@:
    srli    a10, \XH, 20
    beq     a10, a6, .Lsr\@
    extui   a10, \XL, 0, 1           /* binade doubled: S >> 1, then put the exponent (and */
    ssai    1                        /* sign) back: + (top12 + 1) << 19; the out bit rounds */
    src     \XL, \XH, \XL
    srli    \XH, \XH, 1
    addi    a8, a6, 1
    slli    a8, a8, 19
    add     \XH, \XH, a8
    ball    \XH, a7, .Lsinf\@        /* overflow */
    beqz    a10, .Lsx\@
    addi    \XL, \XL, 1
    bnez    \XL, .Lsc2\@
    addi    \XH, \XH, 1
.Lsc2\@:
    bnez    a9, .Lsx\@
    srli    \XL, \XL, 1              /* exactly half: back to even */
    slli    \XL, \XL, 1
.Lsx\@:
    EXIT    \XL, \XH
.Lsinf\@:
    srli    \XH, a6, 11              /* sign from X: the sum may have carried into it */
    slli    \XH, \XH, 31
    or      \XH, \XH, a7
    movi    \XL, 0
    EXIT    \XL, \XH
.Lsr\@:
    ROUND_G \XL, \XH
    ALIGN_Y_BIG \YL, \YH, .Lsb\@, .Lsa\@
.endm

/* Different signs. a6 = X exponent (11 bits), a8 = d >= 0. Result sign = X's. */
.macro DIFF_SIGN XL, XH, YL, YH
    bltui   a8, 2, .Ldc\@
    srli    a10, a8, 6
    bnez    a10, .Ldx\@              /* d >= 64 */
    srli    a6, \XH, 20              /* top12 */
    ALIGN_Y \YL, \YH, .Ldb\@, .Lda\@
    bgeu    \XL, \YL, .Ld1\@         /* packed X - Y' - g */
    addi    \XH, \XH, -1
.Ld1\@:
    sub     \XL, \XL, \YL
    sub     \XH, \XH, \YH
    beqz    a9, .Ld2\@
    neg     a9, a9                   /* fraction left after the borrow */
    bnez    \XL, .Ld3\@
    addi    \XH, \XH, -1
.Ld3\@:
    addi    \XL, \XL, -1
.Ld2\@:
    srli    a10, \XH, 20
    beq     a10, a6, .Ld4\@
    ssai    31                       /* exponent dropped by one (d >= 2: never more, and X's */
    src     \XH, \XH, \XL            /* exponent >= 3, so never denormal): 2 S - top12 << 52 */
    src     \XL, \XL, a9
    slli    a9, a9, 1
    slli    a8, a6, 20
    sub     \XH, \XH, a8
.Ld4\@:
    ROUND_G \XL, \XH
.Ldx\@:
    EXIT    \XL, \XH
    ALIGN_Y_BIG \YL, \YH, .Ldb\@, .Lda\@
.Ldc\@:                              /* d = 0 or 1: exact difference D = X - Y (X << 1 if d = 1) */
    srli    a11, \XH, 31
    slli    a11, a11, 31             /* sign */
    or      \XH, \XH, a7
    slli    \XH, \XH, 11
    srli    \XH, \XH, 11             /* X mantissa */
    or      \YH, \YH, a7
    slli    \YH, \YH, 11
    srli    \YH, \YH, 11             /* Y mantissa */
    beqz    a8, .Ldc1\@
    ssai    31
    src     \XH, \XH, \XL
    slli    \XL, \XL, 1
.Ldc1\@:
    sub     a6, a6, a8               /* exponent of D's bit 52 */
    bgeu    \XL, \YL, .Ldc2\@
    addi    \XH, \XH, -1
.Ldc2\@:
    sub     \XL, \XL, \YL
    sub     \XH, \XH, \YH
    bgez    \XH, .Ldc3\@
    neg     \XL, \XL                 /* |Y| > |X| (d = 0 only): negate, Y's sign */
    neg     \XH, \XH
    beqz    \XL, .Ldc4\@
    addi    \XH, \XH, -1
.Ldc4\@:
    movi    a10, 1
    slli    a10, a10, 31
    xor     a11, a11, a10
.Ldc3\@:
    or      a10, \XH, \XL
    beqz    a10, .Ldzero\@           /* x - x = +0 */
    bbci    \XH, 21, .Ldn\@          /* D < 2^53: exact, normalise left */
    extui   a10, \XL, 0, 1           /* D >= 2^53 (d = 1): one shift right, tie to even */
    ssai    1
    src     \XL, \XH, \XL
    srli    \XH, \XH, 1
    addi    a6, a6, 1
    beqz    a10, .Ldp\@
    bbci    \XL, 0, .Ldp\@
    addi    \XL, \XL, 1
    bnez    \XL, .Ldp\@
    addi    \XH, \XH, 1
    j       .Ldp\@
.Ldn\@:
    nsau    a8, \XH                  /* shift = leading zeros - 11 */
    bnez    \XH, .Ldn1\@
    nsau    a10, \XL
    add     a8, a8, a10
.Ldn1\@:
    addi    a8, a8, -11
    sub     a10, a6, a8              /* resulting exponent */
    bgei    a10, 1, .Ldn2\@
    addi    a8, a6, -1               /* below 2^-1022: stop at exponent 1, pack as denormal */
    movi    a6, 1
    j       .Ldn3\@
.Ldn2\@:
    mov     a6, a10
.Ldn3\@:
    beqz    a8, .Ldp\@
    bgeui   a8, 32, .Ldn4\@
    ssl     a8
    src     \XH, \XH, \XL
    sll     \XL, \XL
    j       .Ldp\@
.Ldn4\@:
    addi    a8, a8, -32
    ssl     a8
    sll     \XH, \XL
    movi    \XL, 0
.Ldp\@:                              /* exponent - 1 + mantissa (implicit bit carries in) */
    addi    a6, a6, -1
    slli    a6, a6, 20
    add     \XH, \XH, a6
    or      \XH, \XH, a11
    EXIT    \XL, \XH
.Ldzero\@:
    movi    a2, 0
    movi    a3, 0
    ret
.endm

    .align  4
.Ladd_slow:                          /* zero, denormal, inf, NaN: the C code */
    movi    a8, ndf_dadd_slow
    jx      a8

    .global __subdf3
    .type   __subdf3, @function
    .align  4
__subdf3:
    movi    a8, 1
    slli    a8, a8, 31
    xor     a5, a5, a8               /* x - y = x + (-y) */
    .global __adddf3
    .type   __adddf3, @function
    .align  4                        /* call0 target; __subdf3 falls through the padding */
__adddf3:
    movi    a7, 0x7FF
    slli    a7, a7, 20
    ball    a3, a7, .Ladd_slow       /* inf / NaN */
    ball    a5, a7, .Ladd_slow
    bnone   a3, a7, .Ladd_slow       /* zero / denormal */
    bnone   a5, a7, .Ladd_slow
    xor     a10, a3, a5
    bltz    a10, .Lfdiff
    srli    a6, a3, 20               /* sign + exponent, 12 bits (signs equal) */
    srli    a8, a5, 20
    sub     a8, a6, a8               /* d; negative: y has the larger exponent */
    bltz    a8, .Lfsy
    SAME_SIGN a2, a3, a4, a5
.Lfsy:
    neg     a8, a8
    srli    a6, a5, 20
    SAME_SIGN a4, a5, a2, a3
.Lfdiff:
    extui   a6, a3, 20, 11
    extui   a10, a5, 20, 11
    sub     a8, a6, a10
    bltz    a8, .Lfdy
    DIFF_SIGN a2, a3, a4, a5
.Lfdy:
    neg     a8, a8
    mov     a6, a10
    DIFF_SIGN a4, a5, a2, a3
    .size   __adddf3, . - __adddf3

/* ---- mul --------------------------------------------------------------------
 * P = (xh:xl) * (yh:yl), 53 x 53 bits, as words w3:w2:w1:w0. */
    .global __muldf3
    .type   __muldf3, @function
    .align  4
__muldf3:
    extui   a6, a3, 20, 11
    extui   a7, a5, 20, 11
    movi    a9, 2046
    addi    a8, a6, -1
    bgeu    a8, a9, .Lmul_slow
    addi    a8, a7, -1
    bgeu    a8, a9, .Lmul_slow
    addi    a1, a1, -16
    s32i    a12, a1, 0
    s32i    a13, a1, 4
    s32i    a14, a1, 8
    add     a6, a6, a7               /* e = ea + eb - 1023 (+1 if P >= 2^105) */
    addmi   a6, a6, -1024
    addi    a6, a6, 1
    movi    a8, 1
    slli    a8, a8, 20
    slli    a9, a3, 12
    srli    a9, a9, 12
    or      a9, a9, a8               /* xh */
    slli    a10, a5, 12
    srli    a10, a10, 12
    or      a10, a10, a8             /* yh */
    mull    a11, a2, a4              /* w0 = lo(xl*yl) */
    muluh   a12, a2, a4              /* s1 = hi(xl*yl) */
    movi    a14, 0                   /* c1 */
    mull    a7, a2, a10
    add     a12, a12, a7             /* + lo(xl*yh) */
    bgeu    a12, a7, 1f
    addi    a14, a14, 1
1:  mull    a7, a9, a4
    add     a12, a12, a7             /* + lo(xh*yl) -> w1 */
    bgeu    a12, a7, 2f
    addi    a14, a14, 1
2:  muluh   a13, a2, a10             /* s2 = hi(xl*yh) + hi(xh*yl) + lo(xh*yh) + c1 */
    muluh   a7, a9, a4
    add     a13, a13, a7             /* both < 2^21: no carry */
    add     a13, a13, a14
    mull    a7, a9, a10
    muluh   a8, a9, a10              /* w3 = hi(xh*yh) + c2 */
    add     a13, a13, a7             /* -> w2 */
    bgeu    a13, a7, 3f
    addi    a8, a8, 1
3:  bbci    a8, 9, 4f                /* P < 2^105: keep = P >> 52 */
    addi    a6, a6, 1                /* keep = P >> 53 */
    ssai    21
    src     a9, a8, a13              /* keep hi */
    src     a10, a13, a12            /* keep lo */
    slli    a7, a12, 11              /* round bit at 31, sticky bits below */
    j       5f
4:  ssai    20
    src     a9, a8, a13
    src     a10, a13, a12
    slli    a7, a12, 12
5:  bgez    a7, .Lmpack              /* round bit clear */
    slli    a7, a7, 1
    or      a7, a7, a11
    bnez    a7, 6f                   /* above half */
    bbci    a10, 0, .Lmpack          /* exactly half: to even */
6:  addi    a10, a10, 1
    bnez    a10, .Lmpack
    addi    a9, a9, 1
.Lmpack:
    addi    a8, a6, -1
    movi    a7, 2046
    bgeu    a8, a7, .Lmrange
    slli    a8, a8, 20
    add     a9, a9, a8
    xor     a7, a3, a5
    extui   a7, a7, 31, 1
    slli    a7, a7, 31
    or      a3, a9, a7
    mov     a2, a10
    l32i    a12, a1, 0
    l32i    a13, a1, 4
    l32i    a14, a1, 8
    addi    a1, a1, 16
    ret
.Lmrange:
    l32i    a12, a1, 0
    l32i    a13, a1, 4
    l32i    a14, a1, 8
    addi    a1, a1, 16
.Lmul_slow:
    movi    a8, ndf_dmul_slow
    jx      a8
    .size   __muldf3, . - __muldf3
#endif
