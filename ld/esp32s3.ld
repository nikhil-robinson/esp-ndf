/*
 * ESP32-S3 Bare Metal Linker Script
 * Memory layout for ESP32-S3
 */

/* Memory Map */
IRAM0_BASE = 0x40370000;
IRAM0_SIZE = 0x8000;        /* 32KB */
DRAM0_BASE = 0x3FC88000;
DRAM0_SIZE = 0x78000;       /* 480KB */
STACK_SIZE = 0x4000;        /* 16KB */

MEMORY
{
    iram_seg (RWX) : ORIGIN = IRAM0_BASE, LENGTH = IRAM0_SIZE
    dram_seg (RW)  : ORIGIN = DRAM0_BASE, LENGTH = DRAM0_SIZE
}

/* Entry point */
ENTRY(_start)

SECTIONS
{
    /* Reset vector - must be at start of IRAM */
    .vector.table : ALIGN(4)
    {
        _vector_start = .;
        *(.vector.table)
        _vector_end = .;
    } > iram_seg

    /* Exception/Interrupt vectors - 256-byte aligned */
    .vector_table : ALIGN(256)
    {
        _vector_table = .;
        *(.vector_table)
    } > iram_seg

    /* Code section */
    .text : ALIGN(4)
    {
        _stext = .;
        *(.entry.text)
        *(.init.literal)
        *(.init)
        *(.text .text.*)
        *(.literal .literal.*)
        _etext = .;
    } > iram_seg

    /* Read-only data */
    .rodata : ALIGN(4)
    {
        _srodata = .;
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        _erodata = .;
    } > dram_seg

    /* Initialized data - source location for copy */
    _sdata_src = LOADADDR(.data);
    
    /* Initialized data */
    .data : ALIGN(4)
    {
        _sdata = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        _edata = .;
    } > dram_seg

    /* Uninitialized data */
    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        _ebss = .;
    } > dram_seg

    /* Stack */
    .stack (NOLOAD) : ALIGN(16)
    {
        _stack_bottom = .;
        . = . + STACK_SIZE;
        _stack_top = .;
    } > dram_seg

    /* Discard unwanted sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }
}
