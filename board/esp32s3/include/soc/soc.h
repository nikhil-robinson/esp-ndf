/* =============================================================================
 * soc.h  –  ESP32-S3 peripheral base addresses and bus topology
 *
 * All addresses from: ESP32-S3 Technical Reference Manual v1.4
 *                     Section 4 (System and Memory Map)
 *
 * Address space overview:
 *   0x3C000000 – 0x3D7FFFFF  DROM (64MB, flash data via D-cache)
 *   0x3FC88000 – 0x3FCFFFFF  Internal DRAM (data SRAM)
 *   0x40000000 – 0x4003FFFF  Internal ROM (256KB)
 *   0x40370000 – 0x403BFFFF  Internal IRAM (code SRAM)
 *   0x42000000 – 0x43FFFFFF  IROM (64MB, flash code via I-cache)
 *   0x50000000 – 0x50001FFF  RTC slow memory (8KB)
 *   0x600FE000 – 0x600FFFFF  RTC fast memory (8KB)
 *   0x60000000 – 0x600FFFFF  Peripheral bus (AHB/AXI)
 * ============================================================================= */
#ifndef ESP32S3_SOC_H
#define ESP32S3_SOC_H

#include "types.h"

/* ---- Register access macros --------------------------------------------- */
/** Read a 32-bit MMIO register */
#define REG_READ(addr)        (*(volatile uint32_t *)(addr))
/** Write a 32-bit MMIO register */
#define REG_WRITE(addr, v)    (*(volatile uint32_t *)(addr) = (v))
/** Read-modify-write: set bits */
#define REG_SET_BIT(r, b)     REG_WRITE((r), REG_READ((r)) | (b))
/** Read-modify-write: clear bits */
#define REG_CLR_BIT(r, b)     REG_WRITE((r), REG_READ((r)) & ~(b))
/** Read-modify-write: set field */
#define REG_SET_FIELD(r, f, v) \
    REG_WRITE((r), (REG_READ((r)) & ~(f##_M)) | (((v) << (f##_S)) & (f##_M)))
/** Read field from register value in-hand */
#define REG_GET_FIELD(rv, f)  (((rv) & (f##_M)) >> (f##_S))

/* ---- Peripheral base addresses (Table 4-1 in ESP32-S3 TRM) -------------- */

/* SYSTEM (APB peripheral bus control, reset, DMA) */
#define DR_REG_SYSTEM_BASE          0x600C0000UL

/* Interrupt Matrix */
#define DR_REG_INTERRUPT_BASE       0x600C2000UL

/* GDMA (General DMA controller) */
#define DR_REG_GDMA_BASE            0x6003F000UL

/* UART */
#define DR_REG_UART0_BASE           0x60000000UL
#define DR_REG_UART1_BASE           0x60010000UL
#define DR_REG_UART2_BASE           0x6002E000UL

/* SPI */
#define DR_REG_SPI0_BASE            0x60003000UL   /* internal flash controller */
#define DR_REG_SPI1_BASE            0x60002000UL   /* internal flash controller */
#define DR_REG_SPI2_BASE            0x60024000UL   /* general-purpose SPI2      */
#define DR_REG_SPI3_BASE            0x60025000UL   /* general-purpose SPI3      */

/* I2C */
#define DR_REG_I2C_BASE             0x60013000UL
#define DR_REG_I2C1_BASE            0x60027000UL

/* I2S */
#define DR_REG_I2S0_BASE            0x6000F000UL
#define DR_REG_I2S1_BASE            0x6002D000UL

/* GPIO */
#define DR_REG_GPIO_BASE            0x60004000UL

/* IO MUX */
#define DR_REG_IO_MUX_BASE          0x60009000UL

/* RTC controller */
#define DR_REG_RTCCNTL_BASE         0x60008000UL

/* Timers */
#define DR_REG_TIMG0_BASE           0x6001F000UL
#define DR_REG_TIMG1_BASE           0x60020000UL

/* RNG */
#define DR_REG_RNG_BASE             0x600B0C00UL   /* inside APB_CTRL_BASE */

/* SHA / AES / RSA (crypto) */
#define DR_REG_SHA_BASE             0x6003B000UL
#define DR_REG_AES_BASE             0x6003A000UL
#define DR_REG_RSA_BASE             0x6003C000UL

/* USB serial/JTAG */
#define DR_REG_USB_SERIAL_JTAG_BASE 0x60038000UL

/* RMT (infrared remote) */
#define DR_REG_RMT_BASE             0x60016000UL

/* LEDC (LED PWM) */
#define DR_REG_LEDC_BASE            0x60019000UL

/* MCPWM */
#define DR_REG_MCPWM0_BASE          0x6001E000UL

/* PCNT (pulse counter) */
#define DR_REG_PCNT_BASE            0x60017000UL

/* SDMMC */
#define DR_REG_SDMMC_BASE           0x60028000UL

/* TWAI (CAN) */
#define DR_REG_TWAI_BASE            0x6002B000UL

/* eFuse */
#define DR_REG_EFUSE_BASE           0x60007000UL

/* Flash SPI1 (QPI/OPI) */
#define DR_REG_SPI_MEM_BASE         0x60003000UL   /* SPI0 (cache) */

/* SYSTIMER (high-resolution 52-bit counter) */
#define DR_REG_SYSTIMER_BASE        0x60023000UL

/* APB_CTRL (miscellaneous APB control) */
#define DR_REG_APB_CTRL_BASE        0x60026000UL

/* ---- Memory region defines (used by startup code and linker) ------------ */
#define SOC_IRAM_LOW           0x40370000UL
#define SOC_IRAM_HIGH          0x403C0000UL
#define SOC_DRAM_LOW           0x3FC88000UL
#define SOC_DRAM_HIGH          0x3FD00000UL
#define SOC_IROM_LOW           0x42000000UL
#define SOC_IROM_HIGH          0x44000000UL
#define SOC_DROM_LOW           0x3C000000UL
#define SOC_DROM_HIGH          0x3E000000UL
#define SOC_RTC_IRAM_LOW       0x600FE000UL
#define SOC_RTC_IRAM_HIGH      0x60100000UL
#define SOC_RTC_DRAM_LOW       0x600FE000UL
#define SOC_RTC_DRAM_HIGH      0x60100000UL
#define SOC_RTC_DATA_LOW       0x50000000UL
#define SOC_RTC_DATA_HIGH      0x50002000UL

/* ---- CPU IDs ------------------------------------------------------------- */
#define PRO_CPU_NUM    0
#define APP_CPU_NUM    1

#endif /* ESP32S3_SOC_H */
