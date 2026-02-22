/* =============================================================================
 * xtensa_ops.h  –  Xtensa LX7 special-register and misc inline assembly macros
 *
 * Provides:
 *   RSR(sr)      – read  special/user register into a C unsigned int
 *   WSR(sr, val) – write special/user register from a C expression
 *   XSR(sr, val) – exchange (read-modify) special/user register
 *   ISYNC()      – instruction-stream synchronisation (after WSR affecting decode)
 *   DSYNC()      – data-side synchronisation (after Store / cache operations)
 *
 * Special register numbering (Xtensa LX7):
 *   See Xtensa ISA Reference §5.3 for the full table.
 *   The numbers used in RSR/WSR instructions are the actual SR numbers.
 * ============================================================================= */
#ifndef XTENSA_OPS_H
#define XTENSA_OPS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- C-level special register accessors --------------------------------- */
#ifdef __ASSEMBLER__

/* In assembly context, just provide numeric aliases */
#define LBEG        0
#define LEND        1
#define LCOUNT      2
#define SAR         3
#define BR          4
#define LITBASE     5
#define SCOMPARE1   12
#define ACCLO       16
#define ACCHI       17
#define M0          32
#define M1          33
#define M2          34
#define M3          35
#define WINDOWBASE  72
#define WINDOWSTART 73
#define IBREAKENABLE 96
#define MEMCTL      97
#define CACHEATTR   98
#define ATOMCTL     99
#define IBREAKA0    128
#define IBREAKA1    129
#define DBREAKA0    144
#define DBREAKA1    145
#define DBREAKC0    160
#define DBREAKC1    161
#define CONFIGID0   176
#define EPC1        177
#define EPC2        178
#define EPC3        179
#define EPC4        180
#define EPC5        181
#define EPC6        182
#define EPC7        183
#define DEPC        192
#define EPS2        194
#define EPS3        195
#define EPS4        196
#define EPS5        197
#define EPS6        198
#define EPS7        199
#define CONFIGID1   208
#define EXCSAVE1    209
#define EXCSAVE2    210
#define EXCSAVE3    211
#define EXCSAVE4    212
#define EXCSAVE5    213
#define EXCSAVE6    214
#define EXCSAVE7    215
#define CPENABLE    224
#define INTSET      226
#define INTCLEAR    227
#define INTENABLE   228
#define PS          230
#define VECBASE     231
#define EXCCAUSE    232
#define DEBUGCAUSE  233
#define CCOUNT      234
#define PRID        235
#define ICOUNT      236
#define ICOUNTLEVEL 237
#define EXCVADDR    238
#define CCOMPARE0   240
#define CCOMPARE1   241
#define CCOMPARE2   242
#define MISC0       244
#define MISC1       245
#define MISC2       246
#define MISC3       247

#else /* C source context */

/**
 * @brief  Read a Xtensa special register.
 * @param  sr  Special register name (unquoted, e.g. PS, VECBASE, INTENABLE).
 */
#define RSR(sr)                                                         \
    ({                                                                  \
        unsigned int __val;                                             \
        __asm__ volatile ("rsr    %0, " #sr : "=a"(__val));             \
        __val;                                                          \
    })

/**
 * @brief  Write a Xtensa special register.
 * @param  sr   Special register name.
 * @param  val  Value to write (converted to uint32_t).
 */
#define WSR(sr, val)                                                    \
    do {                                                                \
        unsigned int __v = (unsigned int)(val);                         \
        __asm__ volatile ("wsr    %0, " #sr :: "a"(__v) : "memory");   \
    } while(0)

/**
 * @brief  Exchange (read then write) a Xtensa special register.
 * @param  sr   Special register name.
 * @param  val  Pointer to uint32_t; on return holds old SR value.
 */
#define XSR(sr, val)                                                    \
    do {                                                                \
        unsigned int __v = (unsigned int)*(val);                        \
        __asm__ volatile ("xsr    %0, " #sr : "+a"(__v) :: "memory");  \
        *(val) = __v;                                                   \
    } while(0)

/**
 * @brief  Instruction-stream synchronisation.
 * Required after WSR to SR affecting instruction fetching (PS, VECBASE, etc.)
 */
#define ISYNC()  __asm__ volatile ("isync" ::: "memory")

/**
 * @brief  Data-side synchronisation (ensures all memory ops are committed).
 * Required after MEMW-class operations and cache control writes.
 */
#define DSYNC()  __asm__ volatile ("dsync" ::: "memory")

/**
 * @brief  RSYNC: ensures SR write side effects are visible.
 * Equivalent to: isync; dsync
 */
#define RSYNC()  __asm__ volatile ("rsync" ::: "memory")

/* ---- Interrupt enable / disable ----------------------------------------- */

/**
 * @brief  Disable all interrupts by setting INTLEVEL=15 in PS.
 * @return Old PS value (for later restore via xtensa_intr_restore).
 */
static inline unsigned int xtensa_intr_disable(void)
{
    unsigned int ps;
    __asm__ volatile (
        "rsil   %0, 15"   /* read PS into %0, then set INTLEVEL=15 */
        : "=a"(ps)
        :
        : "memory"
    );
    return ps;
}

/**
 * @brief  Enable all interrupts (set INTLEVEL=0 in PS).
 */
static inline void xtensa_intr_enable(void)
{
    __asm__ volatile (
        "rsil   a2, 0"    /* set INTLEVEL=0, discard old value */
        ::: "a2", "memory"
    );
}

/**
 * @brief  Restore PS to previously saved value.
 * @param  ps  Value previously returned by xtensa_intr_disable().
 */
static inline void xtensa_intr_restore(unsigned int ps)
{
    WSR(PS, ps);
    RSYNC();
}

/* ---- Cycle counter ------------------------------------------------------- */
/**
 * @brief  Read the CPU cycle counter (CCOUNT register).
 * Wraps every ~54 seconds at 240 MHz.  Useful for coarse timing.
 */
static inline unsigned int xtensa_ccount(void)
{
    return RSR(CCOUNT);
}

/* ---- MEMW – acquire/release barrier ------------------------------------- */
/**
 * @brief  Ordered memory access barrier (MEMW instruction).
 * Ensures all preceding loads/stores complete before subsequent accesses.
 * Equivalent to a hardware acquire/release fence.
 */
#define XTENSA_MEMW()  __asm__ volatile ("memw" ::: "memory")

#endif /* __ASSEMBLER__ */

#ifdef __cplusplus
}
#endif

#endif /* XTENSA_OPS_H */
