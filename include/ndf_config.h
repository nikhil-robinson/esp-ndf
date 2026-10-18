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
#ifndef NDF_CONSOLE
#define NDF_CONSOLE             NDF_CONSOLE_USJ
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

/* ---- set by the build from app.mk (defaults shown) ------------------------- */
#ifndef NDF_PART_TABLE_ADDR
#define NDF_PART_TABLE_ADDR     0x80000 /* partition table; the app image must end below it */
#endif
