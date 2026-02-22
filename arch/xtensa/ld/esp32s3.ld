/* =============================================================================
 * esp32s3.ld  –  ESP32-S3 bare-metal linker script
 *
 * Memory architecture (Xtensa LX7, dual-core):
 *
 *   ┌──────────────────────────────────────────────┐
 *   │ ROM        0x40000000  256 KB  (read-only ROM)│
 *   ├──────────────────────────────────────────────┤
 *   │ IRAM       0x40370000  320 KB  (Instr. RAM)   │ ← vectors + fast code
 *   ├──────────────────────────────────────────────┤
 *   │ DRAM       0x3FC88000  384 KB  (Data   RAM)   │ ← stack / heap / .data
 *   ├──────────────────────────────────────────────┤
 *   │ IROM       0x42000020  8 MB    (Flash via I$) │ ← general code (.text)
 *   ├──────────────────────────────────────────────┤
 *   │ DROM       0x3C000020  8 MB    (Flash via D$) │ ← read-only data
 *   ├──────────────────────────────────────────────┤
 *   │ RTC_IRAM   0x600FE000  8 KB    (RTC fast RAM) │
 *   ├──────────────────────────────────────────────┤
 *   │ RTC_SLOW   0x50000000  8 KB    (RTC slow RAM) │
 *   └──────────────────────────────────────────────┘
 *
 * NOTE on flash offsets:
 *   The ROM MMU maps the app flash partition (default 0x10000) such that:
 *     virt 0x42000020 == flash_offset + 0x00     (I-bus)
 *     virt 0x3C000020 == flash_offset + 0x00     (D-bus)
 *   The 0x20 offset is a convenience to satisfy the 64KB MMU page alignment
 *   constraint: (flash_paddr % 0x10000 == virt_addr % 0x10000).
 *
 * NOTE on Xtensa interrupt vectors:
 *   VECBASE must be 0x400-aligned.  All vectors are placed at the beginning of
 *   IRAM so VECBASE == _iram_start == 0x40370000.
 * ============================================================================= */

/* Force little-endian ELF output.
 * Without this, binutils defaults to big-endian xtensa (elf32-xtensa-be),
 * which causes esptool elf2image to reject the ELF (e_machine byte-swap). */
OUTPUT_FORMAT("elf32-xtensa-le")
OUTPUT_ARCH(xtensa)
ENTRY(call_start_cpu0)

/* -------------------------------------------------------------------------
 * Memory map
 * ------------------------------------------------------------------------- */
MEMORY
{
    /*
     * IRAM for CPU0.
     * First 0x8000 bytes are shared with I-cache; the cache uses the high end,
     * so we start at 0x40370000 + ICACHE_SIZE.  For a 32 KB I-cache the first
     * usable IRAM byte is 0x40378000.  We conservatively reserve the full
     * 32 KB here; adjust SRAM_IRAM_ORG to 0x40370000 if you disable the
     * instruction cache.
     */
    iram0  (RWX) : ORIGIN = 0x40378000, LENGTH = 0x48000   /* ~288 KB usable IRAM  */

    /*
     * DRAM (data RAM).
     * Starts at 0x3FC88000 (SRAM1/2 mapped as data bus).
     */
    dram0  (RW)  : ORIGIN = 0x3FC88000, LENGTH = 0x60000   /* 384 KB DRAM          */

    /*
     * IROM: flash instruction region (code runs from here via I-cache).
     * The 0x42000020 origin accounts for the image header and the MMU
     * alignment requirement.
     */
    irom0  (RX)  : ORIGIN = 0x42000020, LENGTH = 0x7FFFE0  /* ~8 MB flash          */

    /*
     * DROM: flash data region (rodata, read-only via D-cache).
     */
    drom0  (R)   : ORIGIN = 0x3C000020, LENGTH = 0x7FFFE0  /* ~8 MB flash          */

    /*
     * RTC fast RAM (executable, persists through deep-sleep).
     */
    rtc_iram (RWX) : ORIGIN = 0x600FE000, LENGTH = 0x2000  /* 8 KB                 */

    /*
     * RTC slow RAM (ULP accessible, persists through deep-sleep).
     */
    rtc_slow (RW)  : ORIGIN = 0x50000000, LENGTH = 0x2000  /* 8 KB                 */
}

/* -------------------------------------------------------------------------
 * Linker-exported symbols used by startup code (start.S / board_init.c)
 * ------------------------------------------------------------------------- */
_iram_start     = ORIGIN(iram0);
_iram_end       = ORIGIN(iram0) + LENGTH(iram0);
_dram_start     = ORIGIN(dram0);
_dram_end       = ORIGIN(dram0) + LENGTH(dram0);

/* -------------------------------------------------------------------------
 * SECTIONS
 * ------------------------------------------------------------------------- */
SECTIONS
{
    /* ======================================================================
     * Xtensa Interrupt / Exception Vector Table
     *
     * VECBASE = 0x40378000.  Must be 0x400-aligned.
     * Vector offsets from VECBASE (Xtensa LX ISA §4.4.1.5):
     *   +0x000  WindowOverflow4    +0x040  WindowUnderflow4
     *   +0x080  WindowOverflow8    +0x0C0  WindowUnderflow8
     *   +0x100  WindowOverflow12   +0x140  WindowUnderflow12
     *   +0x180  Level2 IRQ         +0x1C0  Level3 IRQ
     *   +0x200  Level4 IRQ         +0x240  Level5 IRQ
     *   +0x280  Level6 Debug       +0x2C0  Level7 NMI
     *   +0x300  Kernel exception   +0x340  User exception
     *   +0x3C0  Double exception
     * Total reserved: 0x400 bytes.
     * ====================================================================== */
    .vectors ORIGIN(iram0) :
    {
        _vectors_start = .;
        /* Literal pool for vector code (currently empty; must stay before
         * any .vectors code that uses L32R to satisfy backward-only rule) */
        *(.vectors.literal .vectors.literal.*)
        KEEP(*(.vectors))
        . = ALIGN(0x400);
        _vectors_end = .;
    } > iram0

    /* ======================================================================
     * Code that MUST live in IRAM (ISRs, cache-bypass routines, etc.)
     * ====================================================================== */
    .iram.text :
    {
        _iram_text_start = .;
        /* Literal pools for IRAM code (must be co-located with the code that
         * uses them so that L32R offsets remain in range) */
        *(.iram.literal .iram.literal.*)
        *(.iram.text .iram.text.*)
        . = ALIGN(4);
        _iram_text_end = .;
    } > iram0

    /* ======================================================================
     * Main code section – lives in flash, executed via I-cache (IROM)
     * ====================================================================== */
    .text :
    {
        _text_start = .;
        /* Literal pools for L32R MUST come before the code that uses them
         * (L32R is a backward-only load; literal must be at lower address).
         * .literal.entry holds constants for call_start_cpu0 in .text.entry */
        *(.literal.entry)
        *(.literal .literal.*)
        *(.xt.lit .xt.lit.*)
        /* Entry point stub */
        KEEP(*(.text.entry))
        *(.text .text.* .gnu.linkonce.t.*)
        . = ALIGN(4);
        _text_end = .;
    } > irom0

    /* ======================================================================
     * Read-only data – lives in flash, accessible via D-cache (DROM)
     * ====================================================================== */
    .rodata :
    {
        _rodata_start = .;
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        *(.srodata .srodata.*)
        . = ALIGN(4);
        _rodata_end = .;
    } > drom0

    /* ======================================================================
     * Initialised data (.data)
     *
     * The data lives in flash at LMA (load address) but must be copied to
     * DRAM at VMA (execution address) by startup code.
     * The symbols _data_lma_start / _data_start / _data_end are exported
     * for start.S to perform the flash-to-DRAM copy.
     * ====================================================================== */
    /* Workaround: xtensa-esp-elf-ld (ldlang.c:1057) crashes if SIZEOF() is
     * used on a section with AT > clause, AND if symbols are defined inside
     * such a section. Define _data_start with safe ADDR() and _data_end at
     * the top of .bss (which equals end-of-.data after alignment). */
    .data :
    {
        *(.data .data.* .gnu.linkonce.d.*)
        *(.sdata .sdata.*)
        . = ALIGN(4);
    } > dram0 AT > drom0

    /* Workaround for xtensa-esp-elf-ld (ldlang.c:1057) bug:
     * - SIZEOF() on any output section (AT> or NOLOAD) crashes this ld.
     * - Symbols defined inside a section with AT > also crash.
     * Solution: use dot-based assignments and ADDR() outside section braces.
     * The dot between sections is the VMA end of the preceding output section
     * in the same region, which is what we want for start/end symbols. */
    _data_start     = ADDR(.data);          /* VMA start of .data in DRAM    */
    _data_lma_start = LOADADDR(.data);      /* LMA in flash: source for copy */

    /* _data_end / _bss_start are set right before .bss begins (== after .data) */
    _data_end  = .;
    _bss_start = .;

    /* ======================================================================
     * Zero-initialised data (.bss) – not stored in flash, zeroed by startup
     * ====================================================================== */
    .bss (NOLOAD) :
    {
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(.sbss .sbss.*)
        *(COMMON)
    } > dram0

    _bss_end = .;   /* dot after .bss is at end of .bss in dram0 */

    /* ======================================================================
     * Heap – starts right after .bss, grows upward
     * ====================================================================== */
    .heap (NOLOAD) :
    {
        . = ALIGN(8);
        _heap_start = .;
    } > dram0

    /* ======================================================================
     * Stack – at the top of DRAM, grows downward.
     *
     * Default: 8 KB per core (16 KB total for dual-core future use).
     * _stack_top is loaded into SP by startup code.
     * ====================================================================== */
    _stack_size     = 0x2000;          /* 8 KB per core                       */
    _stack_top      = ORIGIN(dram0) + LENGTH(dram0);   /* top == high address  */
    _stack_bottom   = _stack_top - _stack_size;
    _heap_end       = _stack_bottom;   /* heap ends where stack begins        */

    /* ======================================================================
     * RTC fast RAM sections (survive deep-sleep)
     * ====================================================================== */
    .rtc.text :
    {
        _rtc_text_start = .;
        *(.rtc.text .rtc.text.*)
        . = ALIGN(4);
        _rtc_text_end = .;
    } > rtc_iram

    .rtc.data :
    {
        _rtc_data_start = .;
        *(.rtc.data .rtc.data.*)
        . = ALIGN(4);
        _rtc_data_end = .;
    } > rtc_slow

    /* ======================================================================
     * Debug / metadata sections – silently discard to save space
     * ====================================================================== */
    /DISCARD/ :
    {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.xtensa.info)
        *(.xt.prop .xt.prop.*)
        *(.xt.lit.plt)
    }
}

/* Helpful assertion: catch stack/heap overflow at link time */
ASSERT(_heap_start <= _stack_bottom,
       "ERROR: .bss + heap overflow into stack region!")
