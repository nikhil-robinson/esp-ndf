/* =============================================================================
 * system_reg.h  –  ESP32-S3 SYSTEM / APB_CTRL / CPU control registers
 *
 * The SYSTEM peripheral (base 0x600C0000) controls:
 *   - CPU frequency selection
 *   - Peripheral reset and clock gating
 *   - APB clock divider
 *   - Cache / MMU configuration
 *   - Various system-level features
 *
 * APB_CTRL (base 0x60026000) provides:
 *   - Date/version register
 *   - Clock/power control
 *
 * Source: ESP32-S3 TRM v1.4, Chapter 7 (Reset and Clock)
 * ============================================================================= */
#ifndef ESP32S3_SYSTEM_REG_H
#define ESP32S3_SYSTEM_REG_H

#include "soc/soc.h"

/* ---- SYSTEM register offsets (base = 0x600C0000) ------------------------ */

/* CPU control: reset and select core0/core1 */
#define SYSTEM_CPU_PERI_CLK_EN_REG      (DR_REG_SYSTEM_BASE + 0x00)
#define SYSTEM_CPU_PERI_RST_EN_REG      (DR_REG_SYSTEM_BASE + 0x04)
#define SYSTEM_CPU_PER_CONF_REG         (DR_REG_SYSTEM_BASE + 0x08)

/* Core-1 (APP CPU) control */
#define SYSTEM_CORE_1_CONTROL_0_REG     (DR_REG_SYSTEM_BASE + 0x08)
#define SYSTEM_CONTROL_CORE_1_CLKGATE_EN  BIT(1)
#define SYSTEM_CONTROL_CORE_1_RUNSTALL    BIT(0)
#define SYSTEM_CONTROL_CORE_1_RESETING    BIT(16)

/* Peripheral clock gating register 0 (PERIP_CLK_EN0)
 * Bit N = 1: clock enabled for that peripheral
 */
#define SYSTEM_PERIP_CLK_EN0_REG        (DR_REG_SYSTEM_BASE + 0x18)
/* Peripheral clock gating register 1 */
#define SYSTEM_PERIP_CLK_EN1_REG        (DR_REG_SYSTEM_BASE + 0x1C)
/* Peripheral reset register 0: write 1 to hold peripheral in reset */
#define SYSTEM_PERIP_RST_EN0_REG        (DR_REG_SYSTEM_BASE + 0x20)
/* Peripheral reset register 1 */
#define SYSTEM_PERIP_RST_EN1_REG        (DR_REG_SYSTEM_BASE + 0x24)

/* ---- PERIP_CLK_EN0 / PERIP_RST_EN0 bit definitions --------------------- */
#define SYSTEM_TIMERS_CLK_EN            BIT(0)
#define SYSTEM_SPI01_CLK_EN             BIT(1)
#define SYSTEM_UART_CLK_EN              BIT(2)
#define SYSTEM_WDG_CLK_EN              BIT(3)
#define SYSTEM_I2S0_CLK_EN              BIT(4)
#define SYSTEM_UART1_CLK_EN             BIT(5)
#define SYSTEM_SPI2_CLK_EN              BIT(6)
#define SYSTEM_I2C_EXT0_CLK_EN          BIT(7)
#define SYSTEM_UHCI0_CLK_EN             BIT(8)
#define SYSTEM_RMT_CLK_EN               BIT(9)
#define SYSTEM_PCNT_CLK_EN              BIT(10)
#define SYSTEM_LEDC_CLK_EN              BIT(11)
#define SYSTEM_UHCI1_CLK_EN             BIT(12)
#define SYSTEM_TIMERGROUP_CLK_EN        BIT(13)
#define SYSTEM_EFUSE_CLK_EN             BIT(14)
#define SYSTEM_TIMERGROUP1_CLK_EN       BIT(15)
#define SYSTEM_SPI3_CLK_EN              BIT(16)
#define SYSTEM_PWM0_CLK_EN              BIT(17)
#define SYSTEM_I2C_EXT1_CLK_EN          BIT(18)
#define SYSTEM_TWAI_CLK_EN              BIT(19)
#define SYSTEM_PWM1_CLK_EN              BIT(20)
#define SYSTEM_I2S1_CLK_EN              BIT(22)
#define SYSTEM_SPI2_DMA_CLK_EN          BIT(23)
#define SYSTEM_USB_OTG_CLK_EN           BIT(24)
#define SYSTEM_UART_MEM_CLK_EN          BIT(24)
#define SYSTEM_SPI3_DMA_CLK_EN          BIT(25)
#define SYSTEM_PWM2_CLK_EN              BIT(26)
#define SYSTEM_PWM3_CLK_EN              BIT(27)
#define SYSTEM_SYSTIMER_CLK_EN          BIT(28)
#define SYSTEM_ADC2_ARB_CLK_EN          BIT(29)
#define SYSTEM_SPI4_CLK_EN              BIT(30)

/* ---- PERIP_CLK_EN1 bit definitions -------------------------------------- */
#define SYSTEM_CRYPTO_AES_CLK_EN        BIT(1)
#define SYSTEM_CRYPTO_SHA_CLK_EN        BIT(2)
#define SYSTEM_CRYPTO_RSA_CLK_EN        BIT(3)
#define SYSTEM_CRYPTO_DS_CLK_EN         BIT(4)
#define SYSTEM_CRYPTO_HMAC_CLK_EN       BIT(5)
#define SYSTEM_DMA_CLK_EN               BIT(6)
#define SYSTEM_SDIO_HOST_CLK_EN         BIT(7)
#define SYSTEM_LCD_CAM_CLK_EN           BIT(8)
#define SYSTEM_UART2_CLK_EN             BIT(9)
#define SYSTEM_USB_SERIAL_JTAG_CLK_EN   BIT(10)
#define SYSTEM_MCPWM1_CLK_EN            BIT(11)
#define SYSTEM_PCNT_CLK_EN1             BIT(12)

/* ---- CPU_PER_CONF register bit fields ----------------------------------- */
/*
 * CPUPERIOD_SEL:
 *   0 = 80 MHz (PLL / 3)
 *   1 = 160 MHz (PLL / 1.5)
 *   2 = 240 MHz (PLL / 1) [maximum]
 */
#define SYSTEM_CPUPERIOD_SEL_S      0
#define SYSTEM_CPUPERIOD_SEL_M      (0x3)
#define SYSTEM_CPUPERIOD_80MHZ      0
#define SYSTEM_CPUPERIOD_160MHZ     1
#define SYSTEM_CPUPERIOD_240MHZ     2
#define SYSTEM_PLL_FREQ_SEL_S       2    /* [2] 0=320MHz PLL, 1=480MHz PLL  */
#define SYSTEM_CLK_EN_S             3    /* [3] PLL clock enable             */

/* ---- System misc (bus matrix, BUS_ERR_RESP, etc.) ----------------------- */
#define SYSTEM_BUS_RST_EN_REG       (DR_REG_SYSTEM_BASE + 0x28)
#define SYSTEM_WIFI_CLK_EN_REG      (DR_REG_SYSTEM_BASE + 0x14)
#define SYSTEM_WIFI_RST_EN_REG      (DR_REG_SYSTEM_BASE + 0x18)  /* alias */
#define SYSTEM_EDMA_CTRL_REG        (DR_REG_SYSTEM_BASE + 0x2C)

/* ---- APB_CTRL register offsets (base = 0x60026000) ---------------------- */
#define APB_CTRL_SYSCLK_CONF_REG    (DR_REG_APB_CTRL_BASE + 0x00)
#define APB_CTRL_TICK_CONF_REG      (DR_REG_APB_CTRL_BASE + 0x04)
#define APB_CTRL_DATE_REG           (DR_REG_APB_CTRL_BASE + 0x7C)

/* APB clock divider (APB = CPU_CLK / (APB_CPUPERIOD_SEL+1)) */
#define APB_CTRL_PRE_DIV_CNT_S      0
#define APB_CTRL_PRE_DIV_CNT_M      (0x3FF)
#define APB_CTRL_CLK_320M_EN_S      10
#define APB_CTRL_CLK_EN_S           11

/* ---- APB frequency (after clock tree configuration) --------------------- */
#define APB_CLK_FREQ_HZ     80000000UL    /* APB is always 80 MHz when PLL on */
#define REF_CLK_FREQ_HZ      1000000UL    /* 1 MHz reference clock            */
#define XTAL_CLK_FREQ_HZ    40000000UL    /* External crystal: 40 MHz         */

#endif /* ESP32S3_SYSTEM_REG_H */
