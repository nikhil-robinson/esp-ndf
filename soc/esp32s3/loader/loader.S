/* Fast-boot loader entry (soc/esp32s3/loader/loader.c). The ROM loads only this small image and jumps here;
 * same handover as arch/xtensa/start.S: drop the ROM's windowed state, CALL0 from here on. */
#define PS_INIT  0x2F        /* UM = 1, EXCM = 0, WOE = 0, INTLEVEL = 15 */
    .section .text._loader_entry, "ax"     /* first: the RTC stage must start at the reset vector 0x50000000 */
    .global _loader_entry
    .align 4
_loader_entry:                             /* code at the very first byte: jump over the literal pool */
    j       2f
    .literal_position
    .align 4
2:
    movi    a0, PS_INIT
    wsr     a0, ps
    rsync
    movi    a0, 0
    wsr     a0, windowbase
    rsync
    movi    a0, 1
    wsr     a0, windowstart
    rsync
    movi    a1, _loader_stack_top
    movi    a0, 0
    call0   loader_main
1:  j       1b

#if NDF_LOADER_RTC
/* RTC stage entry at the reset vector 0x50000000 (loader_rtc.ld): straight to the stage in RTC fast memory */
    .section .text._rtc_entry, "ax"
    .global _rtc_entry
    .align 4
_rtc_entry:
    j       2f
    .literal_position
    .align 4
2:  movi    a0, _loader_entry
    jx      a0
#endif
