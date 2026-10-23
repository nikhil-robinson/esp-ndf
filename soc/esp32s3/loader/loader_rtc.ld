/* RTC boot stage (loader.c with NDF_LOADER_RTC). The CPU reset vector 0x50000000 (TRM 10.6) is RTC slow memory, which
 * is several microseconds per access: only a jump (loader.S, .text._rtc_entry) lives there. The stage runs from RTC
 * fast memory 0x600FE000 (CPU-speed, kept across resets, unused by the app). Data and stack in DRAM 0x3FCE8D00, as the
 * flash loader: free while the app is not running yet. build/<app>/rtcboot.bin = "RTCB", jump size, stage size, both. */
ENTRY(_rtc_entry)
MEMORY { lrtc  (RX) : ORIGIN = 0x50000000, LENGTH = 0x100
         lfast (RX) : ORIGIN = 0x600FE000, LENGTH = 0x2000
         ldram (RW) : ORIGIN = 0x3FCE8D00, LENGTH = 0x300 }
SECTIONS
{
    .rtc : { KEEP(*(.text._rtc_entry)) . = ALIGN(4); } > lrtc
    .text : { KEEP(*(.text._loader_entry)) *(.literal .literal.* .text .text.* .rodata .rodata.*) . = ALIGN(4); } > lfast
    .data : { *(.data .data.*) } > ldram
    .bss (NOLOAD) : { *(.bss .bss.* COMMON) . = ALIGN(16); . += 0x200; _loader_stack_top = .; } > ldram
    /DISCARD/ : { *(.comment .note.* .eh_frame .xt.prop .xt.prop.* .xtensa.info) }
}
