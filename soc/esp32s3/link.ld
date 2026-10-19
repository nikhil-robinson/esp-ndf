/* ESP32-S3 RAM-only image, loaded from flash 0x0 by the ROM loader.
 * No flash cache, no MMU: every fetch is single-cycle SRAM -> deterministic.
 * SRAM1 is one memory seen twice: IRAM 0x4037_8000 == DRAM 0x3FC8_8000
 * (offset 0x6F_0000). Code goes first; data starts right after the code's
 * DRAM alias, so no RAM is wasted on a fixed split. Stay below 0x3FCE9000:
 * the ROM loader's own data lives above it. */
ENTRY(_start0)

MEMORY
{
    iram (RWX) : ORIGIN = 0x40378000, LENGTH = 0x3FCE9000 - 0x3FC88000
    dram (RW)  : ORIGIN = 0x3FC88000, LENGTH = 0x3FCE9000 - 0x3FC88000
}

SECTIONS
{
    .text : {
        KEEP(*(.vectors))                 /* first: VECBASE = 0x40378000, 1 KB aligned */
        *(.iram.literal .iram.text .iram.text.*)
        *(.literal .literal.* .text .text.*)
        . = ALIGN(16);
        _text_end = .;
    } > iram

    _data_origin = _text_end - 0x6F0000;  /* DRAM alias of the end of code */

    .data _data_origin : {
        *(.rodata .rodata.* .data .data.* .sdata .sdata.*)
    } > dram

    .bss (NOLOAD) : ALIGN(16) {
        _bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;                     /* stacks stay outside the zeroed range */
        . = ALIGN(16); . += NDF_STACK0_SIZE; _stack0_top = .;   /* sizes from app.mk (--defsym) */
        . = ALIGN(16); . += NDF_STACK1_SIZE; _stack1_top = .;
    } > dram

    /DISCARD/ : { *(.comment .note.* .eh_frame .xt.prop .xt.prop.* .xtensa.info) }
}

ASSERT(_vector_table == 0x40378000, "vector table must be first in IRAM")
ASSERT(_stack1_top <= 0x3FCE9000, "RAM full: code + data + stacks exceed SRAM below the ROM loader area")
/* the baseband dump engine writes SRAM banks 0x3FCB0000..0x3FCDFFFF (drivers/rf/rf_dump.c) */
ASSERT(!DEFINED(rf_dump_burst) || _stack1_top <= 0x3FCB0000, "RF dump engine in use: code + data + stacks must end below 0x3FCB0000")
