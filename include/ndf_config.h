/* esp-ndf configuration: every run-time option, with its default.
 *
 * An application overrides any of them by defining it in <app>/ndf_config.h.
 * The build force-includes that file before everything else. Build-side
 * options (flash header, stack sizes, partition table address) live in
 * <app>/app.mk; see docs/CONFIG.md for the full list.
 *
 * Chip capabilities (what the silicon has) are separate: soc/<chip>/soc_caps.h. */
#pragma once

/* ---- clock ------------------------------------------------------------------ */
#ifndef NDF_CPU_MHZ
#define NDF_CPU_MHZ             240     /* 240, 160, 80 (PLL) or 40 (crystal) */
#endif

/* ---- flash ---------------------------------------------------------------- */
#define NDF_FLASH_READ_AUTO     0       /* data cache + QIO when the chip has QE set, else SPI1 */
#define NDF_FLASH_READ_SPI1     1       /* always the SPI1 register path (cache left alone) */
#ifndef NDF_FLASH_READ
#define NDF_FLASH_READ          NDF_FLASH_READ_AUTO
#endif
#ifndef NDF_FLASH_SUSPEND
#define NDF_FLASH_SUSPEND       1       /* erase/program auto-suspend: 0 off, 1 IDF's chip list + SFDP, 2 force (chip tested) */
#endif
#ifndef NDF_FLASH_SUS_HOLD_US
#define NDF_FLASH_SUS_HOLD_US   50      /* suspend: tSUS, wait after suspend/resume (IDF default 50) */
#endif
#ifndef NDF_RNG_RESEED
#define NDF_RNG_RESEED          65536   /* rng_fill: new hardware entropy into the key after this many bytes */
#endif
#ifndef NDF_FLASH_MHZ
#define NDF_FLASH_MHZ           120     /* flash clock: 80, or 120 (timing tuned at start; falls back to 80) */
#endif
#ifndef NDF_FLASH_QE_SET
#define NDF_FLASH_QE_SET        1       /* set the chip's quad-enable bit if clear (non-volatile status write, once) */
#endif

/* ---- console (printf) ----------------------------------------------------- */
#define NDF_CONSOLE_NONE        0       /* printf output dropped */
#define NDF_CONSOLE_USJ         1       /* USB Serial/JTAG (the devkit's USB port) */
#define NDF_CONSOLE_UART0       2       /* UART0, polled FIFO writes */
#ifndef NDF_CONSOLE
#define NDF_CONSOLE             NDF_CONSOLE_USJ
#endif
#ifndef NDF_CONSOLE_BAUD
#define NDF_CONSOLE_BAUD        115200  /* UART0 console only */
#endif
#ifndef NDF_CONSOLE_TX_PIN
#define NDF_CONSOLE_TX_PIN      43
#endif
#ifndef NDF_CONSOLE_RX_PIN
#define NDF_CONSOLE_RX_PIN      44
#endif

/* ---- code from flash (build option XIP in app.mk; the Makefile passes these) ---------------------------- */
#define NDF_XIP_OFF             0       /* everything in internal RAM */
#define NDF_XIP_LIB             1       /* libraries that never run in interrupts or flash operations in flash */
#define NDF_XIP_ALL             2       /* also the application and the storage stacks */
#ifndef NDF_XIP
#define NDF_XIP                 NDF_XIP_LIB
#endif
#ifndef NDF_XIP_ADDR
#define NDF_XIP_ADDR            0x300000
#endif
#ifndef NDF_ICACHE_KB
#define NDF_ICACHE_KB           32      /* 16 or 32: instruction cache (SRAM0 0x40370000.., below the image) */
#endif

/* ---- UART driver ------------------------------------------------------------ */
#ifndef NDF_UART_RING
#define NDF_UART_RING           2048    /* software ring per direction and port, power of 2 */
#endif

/* ---- boot ------------------------------------------------------------------- */
#ifndef NDF_BOOT_ROM_LOG_OFF
#define NDF_BOOT_ROM_LOG_OFF    1       /* ROM boot log off for resets after the first boot (RTC flag, not an eFuse) */
#endif
#ifndef NDF_BOOT_BANNER
#define NDF_BOOT_BANNER         0       /* 1: print chip/flash/clock/reset info before main() */
#endif
#ifndef NDF_BOOT_WDT_OFF
#define NDF_BOOT_WDT_OFF        1       /* stop the ROM-enabled watchdogs (no WDT driver yet) */
#endif
#ifndef NDF_BOOT_RTC_RESET
#define NDF_BOOT_RTC_RESET      1       /* return RTC pads/touch to power-on state (held pads kept) */
#endif

/* ---- core services --------------------------------------------------------- */
#ifndef NDF_SWTIMER_LEVEL
#define NDF_SWTIMER_LEVEL       1       /* interrupt level of software-timer callbacks (1..3) */
#endif
#define NDF_LOG_NONE 0
#define NDF_LOG_ERROR 1
#define NDF_LOG_WARN 2
#define NDF_LOG_INFO 3
#define NDF_LOG_DEBUG 4
#ifndef NDF_LOG_LEVEL
#define NDF_LOG_LEVEL           NDF_LOG_INFO    /* LOGx() above this level compile to nothing */
#endif
#ifndef NDF_PANIC_RESTART
#define NDF_PANIC_RESTART       0       /* after a fault/assert/watchdog report: 0 halt (debugger), 1 restart */
#endif
#ifndef NDF_ASSERT_ENABLE
#define NDF_ASSERT_ENABLE       1       /* 0: NDF_ASSERT() compiles to nothing */
#endif

/* ---- storage (RAM reserved statically) ------------------------------------- */
#ifndef NDF_VFS_MAX_MOUNTS
#define NDF_VFS_MAX_MOUNTS      4       /* mounted file systems */
#endif
#ifndef NDF_VFS_MAX_FILES
#define NDF_VFS_MAX_FILES       16      /* open files + directories, all file systems */
#endif
#ifndef NDF_FAT_MAX_FILES
#define NDF_FAT_MAX_FILES       8
#endif
#ifndef NDF_FAT_MAX_DIRS
#define NDF_FAT_MAX_DIRS        4
#endif
#ifndef NDF_FAT_PATH_CACHE
#define NDF_FAT_PATH_CACHE      4       /* resolved-path cache entries  */
#endif
#ifndef NDF_SPIFFS_MAX_FILES
#define NDF_SPIFFS_MAX_FILES    4
#endif
#ifndef NDF_SPIFFS_MAX_DIRS
#define NDF_SPIFFS_MAX_DIRS     2
#endif
#ifndef NDF_SPIFFS_MAX_BLOCKS
#define NDF_SPIFFS_MAX_BLOCKS   256     /* largest volume in 4 KB blocks (256 = 1 MB) */
#endif
#ifndef NDF_NVS_CACHE
#define NDF_NVS_CACHE           8       /* recently read items kept  */
#endif

/* ---- set by the build from app.mk (defaults shown) ------------------------- */
#ifndef NDF_PART_TABLE_ADDR
#define NDF_PART_TABLE_ADDR     0x80000 /* partition table; the app image must end below it */
#endif
