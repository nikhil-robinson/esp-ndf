/*
 * ESP32-S3 Startup Code
 * Xtensa LX7 CPU initialization
 * Reset vector, stack init, BSS clear
 */

    .section .vector.table, "ax"
    .align 4
    .global _reset_vector
_reset_vector:
    j       _start

    .section .entry.text, "ax"
    .align 4
    .global _start
    .type _start, @function

_start:
    /* Disable interrupts */
    rsil    a2, 15

    /* Disable watchdog timers FIRST - critical! */
    /* TIMG0 WDT */
    movi    a2, 0x6001F064          /* TIMG0 WDTWPROTECT */
    movi    a3, 0x50D83AA1          /* WDT unlock key */
    s32i    a3, a2, 0
    movi    a2, 0x6001F048          /* TIMG0 WDTCONFIG0 */
    movi    a3, 0
    s32i    a3, a2, 0
    
    /* TIMG1 WDT */
    movi    a2, 0x60020064          /* TIMG1 WDTWPROTECT */
    movi    a3, 0x50D83AA1          /* WDT unlock key */
    s32i    a3, a2, 0
    movi    a2, 0x60020048          /* TIMG1 WDTCONFIG0 */
    movi    a3, 0
    s32i    a3, a2, 0
    
    /* RTC WDT */
    movi    a2, 0x600080A8          /* RTC_CNTL_WDTWPROTECT */
    movi    a3, 0x50D83AA1          /* WDT unlock key */
    s32i    a3, a2, 0
    movi    a2, 0x60008090          /* RTC_CNTL_WDTCONFIG0 */
    movi    a3, 0
    s32i    a3, a2, 0

    /* Super WDT */
    movi    a2, 0x600080AC          /* RTC_CNTL_SWD_WPROTECT */
    movi    a3, 0x8F1D312A          /* SWD unlock key */
    s32i    a3, a2, 0
    movi    a2, 0x600080A4          /* RTC_CNTL_SWD_CONF */
    movi    a3, 0x40000000          /* Disable SWD */
    s32i    a3, a2, 0

    /* Set vector base (VECBASE) */
    movi    a2, _vector_table
    wsr     a2, vecbase
    
    /* Set up the stack pointer */
    movi    a1, _stack_top
    
    /* Set PS for proper operation:
     * - WOE = 0 (window overflow disabled for CALL0 ABI)
     * - CALLINC = 0
     * - UM = 0 (kernel mode)
     * - INTLEVEL = 0
     */
    movi    a2, 0x00000000
    wsr     a2, PS
    rsync

    /* Clear the BSS section */
    movi    a2, _sbss
    movi    a3, _ebss
    movi    a4, 0
.Lclear_bss:
    bge     a2, a3, .Lbss_done
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       .Lclear_bss
.Lbss_done:

    /* Copy initialized data from ROM to RAM if needed */
    /* (Skip if running from RAM or data already in place) */
    movi    a2, _sdata_src
    movi    a3, _sdata
    movi    a4, _edata
    beq     a2, a3, .Ldata_done
.Lcopy_data:
    bge     a3, a4, .Ldata_done
    l32i    a5, a2, 0
    s32i    a5, a3, 0
    addi    a2, a2, 4
    addi    a3, a3, 4
    j       .Lcopy_data
.Ldata_done:

    /* Call system init (optional early init) */
    call0   system_init_early

    /* Jump to main */
    call0   main

    /* If main returns, halt */
.Lhalt:
    waiti   0
    j       .Lhalt

    .size _start, . - _start

/* Weak default for early system init */
    .weak   system_init_early
    .type   system_init_early, @function
system_init_early:
    ret
    .size   system_init_early, . - system_init_early
