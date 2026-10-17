/* ESP32-S3 RAM-only image, loaded from flash 0x0 by the ROM loader.
 * No flash cache, no MMU: every fetch is single-cycle SRAM -> deterministic.
 * IRAM 0x40378000 aliases DRAM 0x3FC88000, so DRAM starts above the IRAM
 * window. Stay below 0x3FCE9700: the ROM loader's data lives above it. */
ENTRY(_start0)

MEMORY
{
    iram (RWX) : ORIGIN = 0x40378000, LENGTH = 128K   /* = DRAM 0x3FC88000-0x3FCA8000 */
    dram (RW)  : ORIGIN = 0x3FCA8000, LENGTH = 0x3FCE9000 - 0x3FCA8000
}

SECTIONS
{
    .text : {
        KEEP(*(.vectors))                 /* first: VECBASE = 0x40378000, 1 KB aligned */
        *(.iram.literal .iram.text .iram.text.*)
        *(.literal .literal.* .text .text.*)
    } > iram

    .data : {
        *(.rodata .rodata.* .data .data.* .sdata .sdata.*)
    } > dram

    .bss (NOLOAD) : ALIGN(16) {
        _bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;                     /* stacks stay outside the zeroed range */
        . = ALIGN(16); . += 16K; _stack0_top = .;
        . = ALIGN(16); . += 16K; _stack1_top = .;
    } > dram

    /DISCARD/ : { *(.comment .note.* .eh_frame .xt.prop .xt.prop.* .xtensa.info) }
}

ASSERT(_vector_table == 0x40378000, "vector table must be first in IRAM")
