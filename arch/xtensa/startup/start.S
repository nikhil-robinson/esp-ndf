/* ===========================================================================
 * start.S  –  ESP32-S3 bare-metal CPU0 reset/startup entry point
 *
 * Called by: ESP32-S3 ROM second-stage bootloader (after loading the app
 *            ELF segments into memory and configuring the MMU/flash cache).
 *
 * Responsibility (in order):
 *   1. Disable watchdog timers inherited from ROM bootloader.
 *   2. Set the VECBASE special register → IRAM start (0x40378000).
 *   3. Initialise the Xtensa PS register:
 *        - Disable all interrupts (INTLEVEL = 15)
 *        - Enable windowed ABI (WOE = 1)
 *        - Set UM = 1 (user-vector mode for exceptions)
 *   4. Clear the windowed register file.
 *   5. Set the stack pointer (SP / a1) to _stack_top.
 *   6. Call __start_c() which:
 *        a. Copies .data from flash (LMA) to DRAM (VMA).
 *        b. Zeroes .bss.
 *        c. Calls board_init() for clock/peripheral init.
 *        d. Calls main().
 *
 * References:
 *   - Xtensa ISA Reference Manual §4.4, §4.7
 *   - ESP32-S3 TRM Chapter 3 (Reset and Clock)
 *   - ESP-IDF components/esp_system/port/cpu_start.c (conceptual reference)
 * =========================================================================== */

#include "xtensa_ops.h"

/* ----- Special register numbers (Xtensa LX7) ----------------------------- */
#define SR_LBEG     0       /* Loop begin         */
#define SR_LEND     1       /* Loop end           */
#define SR_LCOUNT   2       /* Loop count         */
#define SR_SAR      3       /* Shift amount       */
#define SR_BR       4       /* Boolean registers  */
#define SR_LITBASE  5       /* L32R base (extended) */
#define SR_VECBASE  231     /* Vector base        */
#define SR_PS       230     /* Processor status   */
#define SR_WINDOWBASE  72   /* Window base        */
#define SR_WINDOWSTART 73   /* Window start       */

/* ----- PS register bit fields -------------------------------------------- */
/*  PS[3:0]  = INTLEVEL  (0 = all enabled, 15 = all disabled)
 *  PS[4]    = EXCM      (0 = normal,      1 = in-exception)
 *  PS[5]    = UM        (0 = kernel,      1 = user/normal)
 *  PS[7:6]  = RING      (0 = ring 0 / non-secure)
 *  PS[11:8] = OWB       (old window base, written by hardware)
 *  PS[17:16]= CALLINC   (call increment: 0=CALL0, 1=CALL4, 2=CALL8, 3=CALL12)
 *  PS[18]   = WOE       (1 = windowed overflow/underflow enabled)
 */
#define PS_INTLEVEL_DISABLE  0x0F       /* Mask all interrupts        */
#define PS_UM                (1 << 5)   /* User vector mode           */
#define PS_WOE               (1 << 18)  /* Windowed ABI enable        */
#define PS_CALLINC_NONE      0          /* No auto increment (CALL0)  */

/* Startup PS: all interrupts disabled, WOE enabled, user mode */
#define PS_STARTUP   (PS_INTLEVEL_DISABLE | PS_WOE | PS_UM)

/* ----- RTC CNTL / WDT registers (to disable ROM watchdogs) -------------- */
#define RTC_CNTL_BASE           0x60008000
#define RTC_CNTL_WDTWPROTECT    0x00A4      /* WDT write-protect key reg     */
#define RTC_CNTL_WDTCONFIG0     0x0090      /* WDT config                    */
#define WDT_WPROTECT_KEY        0x50D83AA1  /* Magic key to unlock WDT regs  */

#define TIMG0_BASE              0x6001F000
#define TIMG0_WDTWPROTECT       0x00A4
#define TIMG0_WDTCONFIG0        0x008C
#define TIMG0_WDT_KEY           0x50D83AA1

#define TIMG1_BASE              0x60020000
#define TIMG1_WDTWPROTECT       0x00A4
#define TIMG1_WDTCONFIG0        0x008C

/* ===========================================================================
 * mowi  –  pseudo-instruction helper (move wide immediate → 32-bit literal)
 *
 * The Xtensa toolchain provides "movi" for 12-bit sign-extended immediates
 * only.  For 32-bit constants and linker-provided symbols we use l32r.
 * This macro must be defined BEFORE first use.
 * =========================================================================== */
.macro mowi reg, sym
    .literal_position
    l32r    \reg, \sym
.endm

/* ===========================================================================
 * .text.entry section – placed first within IROM text by the linker script.
 * This ensures the entry point (call_start_cpu0) is at the ELF e_entry addr.
 * =========================================================================== */
    .section    .text.entry, "ax"
    .global     call_start_cpu0
    .type       call_start_cpu0, @function
    .align      4

call_start_cpu0:
    /* -----------------------------------------------------------------
     * Step 1: Disable the ROM's watchdog timers so they don't reset us
     *         before we finish initialising.
     * ----------------------------------------------------------------- */

    /* Unlock and disable TIMG0 WDT */
    movi    a2, TIMG0_BASE
    movi    a3, TIMG0_WDT_KEY
    s32i    a3, a2, TIMG0_WDTWPROTECT     /* write protect key unlock */
    s32i    a0, a2, TIMG0_WDTCONFIG0      /* write 0 → disable WDT    */
    s32i    a0, a2, TIMG0_WDTWPROTECT     /* re-lock write protect     */

    /* Unlock and disable TIMG1 WDT */
    movi    a2, TIMG1_BASE
    movi    a3, TIMG0_WDT_KEY
    s32i    a3, a2, TIMG1_WDTWPROTECT
    s32i    a0, a2, TIMG1_WDTCONFIG0
    s32i    a0, a2, TIMG1_WDTWPROTECT

    /* Unlock and disable RTC WDT */
    movi    a2, RTC_CNTL_BASE
    movi    a3, WDT_WPROTECT_KEY
    s32i    a3, a2, RTC_CNTL_WDTWPROTECT
    s32i    a0, a2, RTC_CNTL_WDTCONFIG0
    s32i    a0, a2, RTC_CNTL_WDTWPROTECT

    /* -----------------------------------------------------------------
     * Step 2: Set VECBASE to start of our vector table in IRAM.
     *         _vectors_start is exported by the linker script.
     * ----------------------------------------------------------------- */
    movi    a0, _vectors_start
    wsr     a0, VECBASE
    rsync                               /* ensure SR write is retired */

    /* -----------------------------------------------------------------
     * Step 3: Initialise PS register.
     *   - Disable all interrupts (INTLEVEL=15) until init is done.
     *   - Enable WOE so the windowed ABI works correctly.
     *   - Set UM=1 (user/vector mode for user exception handler).
     * ----------------------------------------------------------------- */
    movi    a0, PS_STARTUP
    wsr     a0, PS
    rsync

    /* -----------------------------------------------------------------
     * Step 4: Clear the windowed register file.
     *   Set WindowBase=0 and WindowStart=0x1 (only window 0 is live).
     *   This prevents spurious window overflow exceptions during init.
     * ----------------------------------------------------------------- */
    movi    a0, 0
    wsr     a0, WINDOWBASE
    rsync
    movi    a0, 1
    wsr     a0, WINDOWSTART
    rsync

    /* -----------------------------------------------------------------
     * Step 5: Zero scratch registers to avoid undefined state traps.
     * ----------------------------------------------------------------- */
    movi    a0, 0
    movi    a1, 0
    movi    a2, 0
    movi    a3, 0
    wsr     a0, LBEG
    wsr     a0, LEND
    wsr     a0, LCOUNT
    wsr     a0, SAR

    /* -----------------------------------------------------------------
     * Step 6: Set the stack pointer.
     *         _stack_top is the top of DRAM (high address → grows down).
     *         Must be 16-byte aligned for the windowed ABI.
     * ----------------------------------------------------------------- */
    mowi    a1, _stack_top      /* a1 == SP */
    movi    a0, 0               /* clear return address (we never return) */

    /* -----------------------------------------------------------------
     * Step 7: Call __start_c() which copies .data, zeros .bss, and
     *         eventually calls main().  Using CALL4 so windowing is active.
     * ----------------------------------------------------------------- */
    call4   __start_c

    /* Should never reach here.  If main() returns, spin. */
.LForeverLoop:
    j       .LForeverLoop

    .size   call_start_cpu0, . - call_start_cpu0


/* ===========================================================================
 * __start_c  –  C-level startup shim (called from above via CALL4)
 *
 * Copies .data from flash to DRAM, zeroes .bss, calls board_init(), main().
 * Written in assembly to avoid C runtime dependencies.
 * =========================================================================== */
    .section .iram.text, "ax"
    .global  __start_c
    .type    __start_c, @function
    .align   4

__start_c:
    /* -- Copy .data from flash (LMA) to DRAM (VMA) --------------------------
     *
     * Symbols provided by the linker script:
     *   _data_lma_start  = LMA (flash address = source)
     *   _data_start      = VMA (DRAM address  = destination)
     *   _data_end        = VMA end
     */
    movi    a2, _data_lma_start      /* source      (flash)   */
    movi    a3, _data_start          /* destination (DRAM)    */
    mowi    a4, _data_end            /* end of destination    */

    /* Check if .data is non-empty */
    beq     a3, a4, .Lbss_clear     /* skip copy if _data_start == _data_end */

.Ldata_copy_loop:
    l32i    a5, a2, 0               /* load word from flash (via D-cache)    */
    s32i    a5, a3, 0               /* store to DRAM                         */
    addi    a2, a2, 4               /* advance source pointer                */
    addi    a3, a3, 4               /* advance destination pointer           */
    bltu    a3, a4, .Ldata_copy_loop

    /* -- Zero .bss ----------------------------------------------------------
     *
     *   _bss_start  = start of BSS in DRAM
     *   _bss_end    = end of BSS in DRAM
     */
.Lbss_clear:
    mowi    a2, _bss_start
    mowi    a3, _bss_end
    movi    a4, 0

    beq     a2, a3, .Lcall_board   /* skip if .bss is empty */

.Lbss_loop:
    s32i    a4, a2, 0               /* write 0 word          */
    addi    a2, a2, 4
    bltu    a2, a3, .Lbss_loop

    /* -- Re-enable interrupts (INTLEVEL=0, WOE still on) --------------------
     * We keep interrupts off until after board_init().
     */

    /* -- Call board_init() --------------------------------------------------
     * board_init() configures clocks, UART, GPIO pull-ups, etc.
     */
.Lcall_board:
    call4   board_init

    /* -- Call main() -------------------------------------------------------- */
    call4   main

    /* -- If main returns, spin forever -------------------------------------- */
.Lhalt:
    j       .Lhalt

    .size   __start_c, . - __start_c
