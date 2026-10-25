/* Fast-boot loader: 4.5 KB just below the ROM loader's data (0x3FCE9000); IRAM 0x403D7E00 == DRAM 0x3FCE7E00. */
ENTRY(_loader_entry)
MEMORY { liram (RX) : ORIGIN = 0x403D7E00, LENGTH = 0xF00
         ldram (RW) : ORIGIN = 0x3FCE8D00, LENGTH = 0x300 }
SECTIONS
{
    .text : { KEEP(*(.text._loader_entry)) *(.literal .literal.* .text .text.*) } > liram
    .data : { *(.rodata .rodata.* .data .data.*) } > ldram
    .bss (NOLOAD) : { *(.bss .bss.* COMMON) . = ALIGN(16); . += 0x200; _loader_stack_top = .; } > ldram   /* not loaded by the ROM */
    /DISCARD/ : { *(.comment .note.* .eh_frame .xt.prop .xt.prop.* .xtensa.info) }
}
