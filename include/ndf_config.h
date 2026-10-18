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

/* ---- boot ------------------------------------------------------------------- */
#ifndef NDF_BOOT_BANNER
#define NDF_BOOT_BANNER         0       /* 1: print chip/flash/clock/reset info before main() */
#endif
#ifndef NDF_BOOT_WDT_OFF
#define NDF_BOOT_WDT_OFF        1       /* stop the ROM-enabled watchdogs (no WDT driver yet) */
#endif
#ifndef NDF_BOOT_RTC_RESET
#define NDF_BOOT_RTC_RESET      1       /* return RTC pads/touch to power-on state (held pads kept) */
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
