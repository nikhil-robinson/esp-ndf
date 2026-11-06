/* ESP32-P4 (v3.x) capabilities: same flags as soc/esp32s3/soc_caps.h, so the public API and every portable fallback
 * stay identical. Chip defaults of ndf_config.h options are set here first (ndf_config.h only fills what is unset).
 * Usable from C and assembly. */
#pragma once

#define SOC_CPU_CORES               2
#define SOC_HAS_CROSS_CORE_INT      1   /* HP_SYSTEM_CPU_INT_FROM_CPU_n */
#define SOC_CPU_RISCV               1
#define SOC_HEAP_LINKER             1   /* heap regions from link.ld (_heap_start/_heap_end, _heap2_*) */

#ifndef NDF_NO_ACCEL
#define SOC_CPU_HAS_FPU             1   /* RV32F */
#define SOC_CPU_HAS_PIE             1   /* vector unit: lib/esp32p4/{dsp,nn}_pie.S, enabled per core in sys.c (v1.x silicon: the v2.1 encodings, soc.mk -march ..._xespv2p1) */
#define SOC_VEC_RAM_LO              0x4FF00000u   /* internal RAM the vector copy / set paths may touch */
#define SOC_VEC_RAM_HI              0x4FFC0000u
#define SOC_DSP_F32_ASM             0   /* float kernels: the compiler on the FPU (PIE has no float) */
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
#define SOC_CPENABLE_BOOT           0
/* crypto engines: the same register layout as the S3 (the shared drivers/crypto files), P4 base addresses; CPU-fed only */
#define SOC_EFUSE_BASE              0x5012D000u   /* eFuse read registers (same block layout as the S3) */
#define SOC_AES_BASE                0x50090000u
#define SOC_SHA_BASE                0x50091000u
#define SOC_RSA_BASE                0x50092000u
#define SOC_CRYPTO_NO_DMA           1   /* the engines with a DMA mode on the P4 say so below */
#define SOC_SHA_DMA                 1   /* SHA blocks by AXI-GDMA (sha.c) */
#define SOC_AES_DMA                 1   /* AES block modes by AXI-GDMA (aes.c) */
#define SOC_ECC_HW                  1   /* ECC and ECDSA engines: P-256 ECDH and signature verification (lib/ecc.c) */
#define SOC_AES_GCM_HW              1   /* the engine's GCM mode (GHASH and tag in hardware) */

/* Silicon revision of the target: 3 (v3.x, ROM data at the top of L2MEM, image at 0x4FF80000) or 1 (v1.x "eco2" ROM, as on the
 * v1.3 boards: the ROM keeps 0x4FF2BBD0-0x4FF40000 and loads images below it; build with P4_REV=1). */
#ifndef NDF_P4_REV
#define NDF_P4_REV 3
#endif
#if NDF_P4_REV >= 3
#define SOC_IMG_LO    0x4FF80000u
#else
#define SOC_IMG_LO    0x4FF00000u
#endif

/* Boot step in start.S before any stack / .bss access: L2 cache = 128 KB, 64 B lines, so 0x4FF20000-0x4FF80000
 * (stacks, .bss, link.ld.in) is plain memory. ROM Cache_Set_L2_Cache_Mode disassembled: one-hot size at CACHE + 0x278
 * (bit 9 = 128 KB, rom/cache.h CACHE_SIZE_128K), one-hot line size at + 0x27C (bit 3 = 64 B). Internal L2MEM goes
 * through the L1 caches only, so nothing cached in L2 needs flushing (nothing external is mapped yet).
 * ponytail: no L2 invalidate - add the SYNC_CTRL sequence once flash / PSRAM are mapped through the cache. */
#if NDF_P4_REV >= 3
#define SOC_BOOT_ASM  li t0, 0x3FF10000; li t1, 0x200; sw t1, 0x278(t0); li t1, 8; sw t1, 0x27C(t0); fence
#else                                   /* v1.x: ROM Cache_Set_L2_Cache_Mode(128K, 8 ways, 64 B) + Cache_Invalidate_All(L2), as IDF cache_hal_init_l2_cache */
#define SOC_BOOT_ASM  li a0, 9; li a1, 8; li a2, 3; li t0, 0x4fc003d4; jalr t0; li a0, 0x20; li t0, 0x4fc00404; jalr t0
#endif

/* chip defaults for ndf_config.h */
#ifndef NDF_CPU_MHZ
#define NDF_CPU_MHZ        400          /* v3 silicon: CPLL 400 MHz (IDF v6.1 default); 200, 100 or 40 (crystal) */
#endif
#ifndef NDF_STACK_GUARD
#define NDF_STACK_GUARD    0            /* hardware stack guard (stackguard.c) is off until checked on the board; 256 arms it */
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

/* PSRAM: 16 / 32 MB HEX on MSPI2; the MMU maps it at 0x48000000 (64 KB pages from entry 0 of the PSRAM MMU) */
#define PSRAM_VADDR            0x48000000u
#define PSRAM_MMU_FIRST        0u
#define SOC_GDMA_PSRAM_TX_OK   1

/* pm.c: CPU frequencies of the clock tree (clk.c clk_cpu_set_mhz) */
#define SOC_PM_MHZ_OK(m)       ((m) == 400 || (m) == 200 || (m) == 100)
#define SOC_PM_MHZ_MAX         400
#define SOC_ULP_MEM_BASE       0x50108000u              /* LP RAM: ulp_mem() words, LP core program */
#define SOC_OTA_NOTE           0x5010FA60u              /* loader note: "OTAS", image flash offset, XIP flash offset (16 bytes at the top of the LP RAM window, link.ld.in) */
#define SOC_LOADER_BASE        0x4FFB9000u              /* BOOT_FAST: ROM-loaded loader window 0x4FFB9000-0x4FFBCFC0; apps end below it */
#define SOC_FLASH_APP_ADDR     0x2000u                  /* the ROM loads the first image from here (BOOT_FAST=0) */
