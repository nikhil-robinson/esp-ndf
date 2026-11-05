/* P4 loader image: loaded by the ROM from flash 0x2000 into the top of the ROM-loadable window (soc_caps.h
 * SOC_LOADER_BASE .. 0x4FFBCFC0); the application image is copied below it. */
ENTRY(_loader_start)
MEMORY { ldr (RWX) : ORIGIN = 0x4FFB9000, LENGTH = 0x3F80 }
SECTIONS
{
    .text : { *(.text.entry) *(.text .text.*) } > ldr
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > ldr
    .data : { __global_pointer$ = . + 0x800; *(.data .data.* .sdata .sdata.*) } > ldr
    .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.* COMMON) } > ldr
    . = ALIGN(16);
    _loader_stack_top = ORIGIN(ldr) + LENGTH(ldr);
    /DISCARD/ : { *(.comment .note.* .eh_frame .riscv.attributes) }
}
ASSERT(. + 2048 <= _loader_stack_top, "loader too large: needs 2 KB of stack")
