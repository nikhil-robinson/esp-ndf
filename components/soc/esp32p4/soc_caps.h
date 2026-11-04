/* ESP32-P4 (v3.x) capabilities: same flags as soc/esp32s3/soc_caps.h, so the public API and every portable fallback
 * stay identical. Chip defaults of ndf_config.h options are set here first (ndf_config.h only fills what is unset).
 * Usable from C and assembly. */
#pragma once

#define SOC_CPU_CORES               2
#define SOC_HAS_CROSS_CORE_INT      1   /* HP_SYSTEM_CPU_INT_FROM_CPU_n */
#define SOC_GPIO_BASE               0x500E0000u
#define SOC_GPIO_IN_HIGH            0x3F  /* GPIO matrix constant-1 / constant-0 input */
#define SOC_GPIO_IN_LOW             0x3E
#define SOC_XC_NOTIFY_REG(core)     (0x500E5010 + 4 * (core))
#define SOC_CPU_RISCV               1
#define SOC_HEAP_LINKER             1   /* heap regions from link.ld (_heap_start/_heap_end, _heap2_*) */

#ifndef NDF_NO_ACCEL
#define SOC_CPU_HAS_FPU             1   /* RV32F */
#define SOC_CPU_HAS_PIE             0   /* Xesppie exists; no esp-ndf kernels for it yet (C paths) */
#define SOC_CPU_HAS_S32C1I          0   /* Xtensa only */
#define SOC_CPU_HAS_AMO             1   /* Zaamo: amoswap / amoadd */
#define SOC_CPU_HWLOOP_LBEG_QUIRK   0
#define SOC_HAS_FLASH_CACHE         0   /* no flash cache / XIP driver on the P4 yet */
#else
#define SOC_CPU_HAS_FPU             1   /* kept: the C ABI of this build (ilp32f) uses the FPU */
#define SOC_CPU_HAS_PIE             0
#define SOC_CPU_HAS_S32C1I          0
#define SOC_CPU_HAS_AMO             0
#define SOC_CPU_HWLOOP_LBEG_QUIRK   0
#define SOC_HAS_FLASH_CACHE         0
#endif
#define SOC_CPENABLE_BOOT           0   /* Xtensa CPENABLE: unused */

/* Boot step in start.S before any stack / .bss access: L2 cache = 128 KB, 64 B lines, so 0x4FF20000-0x4FF80000
 * (stacks, .bss, link.ld.in) is plain memory. ROM Cache_Set_L2_Cache_Mode disassembled: one-hot size at CACHE + 0x278
 * (bit 9 = 128 KB, rom/cache.h CACHE_SIZE_128K), one-hot line size at + 0x27C (bit 3 = 64 B). Internal L2MEM goes
 * through the L1 caches only, so nothing cached in L2 needs flushing (nothing external is mapped yet).
 * ponytail: no L2 invalidate - add the SYNC_CTRL sequence once flash / PSRAM are mapped through the cache. */
#define SOC_BOOT_ASM  li t0, 0x3FF10000; li t1, 0x200; sw t1, 0x278(t0); li t1, 8; sw t1, 0x27C(t0); fence

/* chip defaults for ndf_config.h */
#ifndef NDF_CPU_MHZ
#define NDF_CPU_MHZ        400          /* v3 silicon: CPLL 400 MHz (IDF v6.1 default); 200, 100 or 40 (crystal) */
#endif
#ifndef NDF_CONSOLE
#define NDF_CONSOLE        2            /* NDF_CONSOLE_UART0: the P4 boards' USB-UART bridge; the ROM set it up */
#endif
#ifndef NDF_CONSOLE_TX_PIN
#define NDF_CONSOLE_TX_PIN 37           /* UART0 default pads (ROM) */
#endif
#ifndef NDF_CONSOLE_RX_PIN
#define NDF_CONSOLE_RX_PIN 38
#endif
