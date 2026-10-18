/* ESP32-S3 capabilities. Every accelerated path in esp-ndf is selected by
 * these flags and has a portable fallback, so a chip (or a build) without
 * the feature still compiles and works, only slower.
 * Build with NDF_NO_ACCEL (make NO_ACCEL=1) to force all fallbacks: that is
 * how the fallbacks are tested on this chip. Usable from C and assembly. */
#pragma once

#define SOC_CPU_CORES               2
#define SOC_HAS_CROSS_CORE_INT      1   /* SYSTEM_CPU_INTR_FROM_CPU_n */

#ifndef NDF_NO_ACCEL
#define SOC_CPU_HAS_FPU             1   /* single-precision FPU, coprocessor 0 */
#define SOC_CPU_HAS_PIE             1   /* 128-bit vector unit (EE.*), coprocessor 3 */
#define SOC_CPU_HAS_S32C1I          1   /* compare-and-swap */
#define SOC_CPU_HWLOOP_LBEG_QUIRK   1   /* re-entered LOOP runs slow unless LBEG changes */
#define SOC_HAS_FLASH_CACHE         1   /* SPI0 + MMU + data cache: flash reads via 0x3C000000 */
#else
#define SOC_CPU_HAS_FPU             1   /* kept: the C ABI of this build uses the FPU */
#define SOC_CPU_HAS_PIE             0
#define SOC_CPU_HAS_S32C1I          0
#define SOC_CPU_HWLOOP_LBEG_QUIRK   0
#define SOC_HAS_FLASH_CACHE         0
#endif

/* coprocessor enable mask written at boot (CPENABLE); no ?: so the assembler can use it */
#if SOC_CPU_HAS_FPU && SOC_CPU_HAS_PIE
#define SOC_CPENABLE_BOOT   0x09
#elif SOC_CPU_HAS_FPU
#define SOC_CPENABLE_BOOT   0x01
#elif SOC_CPU_HAS_PIE
#define SOC_CPENABLE_BOOT   0x08
#else
#define SOC_CPENABLE_BOOT   0x00
#endif
