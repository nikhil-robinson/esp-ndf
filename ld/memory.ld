/*
 * ESP32-S3 Memory Map
 * Common memory definitions
 */

/* Internal SRAM 0 - Instruction RAM */
IRAM0_BASE = 0x40370000;
IRAM0_SIZE = 0x8000;        /* 32KB */

/* Internal SRAM 1 - Data RAM */
DRAM0_BASE = 0x3FC88000;
DRAM0_SIZE = 0x78000;       /* 480KB */

/* Internal ROM */
ROM_BASE = 0x40000000;
ROM_SIZE = 0x60000;         /* 384KB */

/* RTC Fast Memory */
RTC_FAST_BASE = 0x600FE000;
RTC_FAST_SIZE = 0x2000;     /* 8KB */

/* Stack size */
STACK_SIZE = 0x4000;        /* 16KB */

MEMORY
{
    iram_seg (RWX) : ORIGIN = IRAM0_BASE, LENGTH = IRAM0_SIZE
    dram_seg (RW)  : ORIGIN = DRAM0_BASE, LENGTH = DRAM0_SIZE
    rtc_seg  (RW)  : ORIGIN = RTC_FAST_BASE, LENGTH = RTC_FAST_SIZE
}
