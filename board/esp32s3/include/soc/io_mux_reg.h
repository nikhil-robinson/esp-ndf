/* =============================================================================
 * io_mux_reg.h  –  ESP32-S3 IO MUX register definitions
 *
 * The IO MUX selects whether a GPIO pad is controlled by:
 *   a) The GPIO matrix (most flexible, all signals routable to any pin)
 *   b) A direct peripheral connection (fast path, bypasses GPIO matrix)
 *
 * Each pad has 1 IO_MUX register controlling:
 *   [12:10] MCU_SEL (function select): 0=GPIO matrix, 1-5=direct peripheral
 *   [9]     FILTER_EN  : noise filter enable
 *   [8]     FUN_WPD    : weak pull-down enable
 *   [7]     FUN_WPU    : weak pull-up enable
 *   [6]     FUN_IE     : input enable (must be set to read GPIO)
 *   [5:4]   FUN_DRV    : drive strength (0=5mA, 1=10mA, 2=20mA, 3=40mA)
 *   [3]     MCU_WPD    : sleep-mode pull-down
 *   [2]     MCU_WPU    : sleep-mode pull-up
 *   [1]     MCU_IE     : sleep-mode input enable
 *   [0]     MCU_OE     : sleep-mode output enable
 *
 * Base: 0x60009000
 * Source: ESP32-S3 TRM v1.4, Chapter 6 (GPIO and IOMUX), Table 6-2
 * ============================================================================= */
#ifndef ESP32S3_IO_MUX_REG_H
#define ESP32S3_IO_MUX_REG_H

#include "soc/soc.h"

/* ---- IO MUX pin register base offsets (Table 6-2 in TRM) ---------------- */
/*
 * IO_MUX_GPIO<n>_REG = DR_REG_IO_MUX_BASE + offset[n]
 * Not all GPIO numbers have contiguous offsets; the table below reflects the
 * physical pad ordering in ESP32-S3.
 */
#define IO_MUX_GPIO0_REG    (DR_REG_IO_MUX_BASE + 0x04)
#define IO_MUX_GPIO1_REG    (DR_REG_IO_MUX_BASE + 0x08)
#define IO_MUX_GPIO2_REG    (DR_REG_IO_MUX_BASE + 0x0C)
#define IO_MUX_GPIO3_REG    (DR_REG_IO_MUX_BASE + 0x10)
#define IO_MUX_GPIO4_REG    (DR_REG_IO_MUX_BASE + 0x14)
#define IO_MUX_GPIO5_REG    (DR_REG_IO_MUX_BASE + 0x18)
#define IO_MUX_GPIO6_REG    (DR_REG_IO_MUX_BASE + 0x1C)
#define IO_MUX_GPIO7_REG    (DR_REG_IO_MUX_BASE + 0x20)
#define IO_MUX_GPIO8_REG    (DR_REG_IO_MUX_BASE + 0x24)
#define IO_MUX_GPIO9_REG    (DR_REG_IO_MUX_BASE + 0x28)
#define IO_MUX_GPIO10_REG   (DR_REG_IO_MUX_BASE + 0x2C)
#define IO_MUX_GPIO11_REG   (DR_REG_IO_MUX_BASE + 0x30)
#define IO_MUX_GPIO12_REG   (DR_REG_IO_MUX_BASE + 0x34)
#define IO_MUX_GPIO13_REG   (DR_REG_IO_MUX_BASE + 0x38)
#define IO_MUX_GPIO14_REG   (DR_REG_IO_MUX_BASE + 0x3C)
#define IO_MUX_GPIO15_REG   (DR_REG_IO_MUX_BASE + 0x40)
#define IO_MUX_GPIO16_REG   (DR_REG_IO_MUX_BASE + 0x44)
#define IO_MUX_GPIO17_REG   (DR_REG_IO_MUX_BASE + 0x48)
#define IO_MUX_GPIO18_REG   (DR_REG_IO_MUX_BASE + 0x4C)
#define IO_MUX_GPIO19_REG   (DR_REG_IO_MUX_BASE + 0x50)
#define IO_MUX_GPIO20_REG   (DR_REG_IO_MUX_BASE + 0x54)
#define IO_MUX_GPIO21_REG   (DR_REG_IO_MUX_BASE + 0x58)
/* GPIO22-25 do not exist on ESP32-S3 */
#define IO_MUX_GPIO26_REG   (DR_REG_IO_MUX_BASE + 0x60)
#define IO_MUX_GPIO27_REG   (DR_REG_IO_MUX_BASE + 0x64)
#define IO_MUX_GPIO28_REG   (DR_REG_IO_MUX_BASE + 0x68)
#define IO_MUX_GPIO29_REG   (DR_REG_IO_MUX_BASE + 0x6C)
#define IO_MUX_GPIO30_REG   (DR_REG_IO_MUX_BASE + 0x70)
#define IO_MUX_GPIO31_REG   (DR_REG_IO_MUX_BASE + 0x74)
#define IO_MUX_GPIO32_REG   (DR_REG_IO_MUX_BASE + 0x78)
#define IO_MUX_GPIO33_REG   (DR_REG_IO_MUX_BASE + 0x7C)
#define IO_MUX_GPIO34_REG   (DR_REG_IO_MUX_BASE + 0x80)
#define IO_MUX_GPIO35_REG   (DR_REG_IO_MUX_BASE + 0x84)
#define IO_MUX_GPIO36_REG   (DR_REG_IO_MUX_BASE + 0x88)
#define IO_MUX_GPIO37_REG   (DR_REG_IO_MUX_BASE + 0x8C)
#define IO_MUX_GPIO38_REG   (DR_REG_IO_MUX_BASE + 0x90)
#define IO_MUX_GPIO39_REG   (DR_REG_IO_MUX_BASE + 0x94)
#define IO_MUX_GPIO40_REG   (DR_REG_IO_MUX_BASE + 0x98)
#define IO_MUX_GPIO41_REG   (DR_REG_IO_MUX_BASE + 0x9C)
#define IO_MUX_GPIO42_REG   (DR_REG_IO_MUX_BASE + 0xA0)
#define IO_MUX_GPIO43_REG   (DR_REG_IO_MUX_BASE + 0xA4)
#define IO_MUX_GPIO44_REG   (DR_REG_IO_MUX_BASE + 0xA8)
#define IO_MUX_GPIO45_REG   (DR_REG_IO_MUX_BASE + 0xAC)
#define IO_MUX_GPIO46_REG   (DR_REG_IO_MUX_BASE + 0xB0)
#define IO_MUX_GPIO47_REG   (DR_REG_IO_MUX_BASE + 0xB4)
#define IO_MUX_GPIO48_REG   (DR_REG_IO_MUX_BASE + 0xB8)

/* ---- IO MUX configuration field shifts --------------------------------- */
#define IO_MUX_MCU_OE_S     0   /* Output enable in sleep mode              */
#define IO_MUX_MCU_IE_S     1   /* Input enable in sleep mode               */
#define IO_MUX_MCU_WPU_S    2   /* Pull-up enable in sleep mode             */
#define IO_MUX_MCU_WPD_S    3   /* Pull-down enable in sleep mode           */
#define IO_MUX_FUN_DRV_S    4   /* Drive strength [5:4]                     */
#define IO_MUX_FUN_IE_S     6   /* Input enable (active high)               */
#define IO_MUX_FUN_WPU_S    7   /* Pull-up enable                           */
#define IO_MUX_FUN_WPD_S    8   /* Pull-down enable                         */
#define IO_MUX_FILTER_EN_S  9   /* Noise filter enable                      */
#define IO_MUX_MCU_SEL_S    10  /* Function select [12:10] – see below      */
#define IO_MUX_SLP_SEL_S    13  /* Sleep mode override enable               */

/* MCU_SEL (function) values:
 *   0 = GPIO Matrix   – most flexible, all signals routable
 *   1 = FUNC1         – direct peripheral connection 1
 *   2 = FUNC2         – direct peripheral connection 2
 *   3 = FUNC3
 *   4 = FUNC4
 *   5 = FUNC5
 * For GPIO mode always use MCU_SEL=2 (the "FUNC2" is GPIO for all pins).
 */
#define IO_MUX_FUNC_GPIO    2   /* Standard GPIO mode for all pins           */
#define IO_MUX_FUNC_UART0   (1) /* UART0 direct function for U0TXD/U0RXD   */

/* Drive strength values */
#define IO_MUX_DRV_5MA      0
#define IO_MUX_DRV_10MA     1
#define IO_MUX_DRV_20MA     2
#define IO_MUX_DRV_40MA     3

/* ---- Helper: set full IO MUX configuration for a pin in GPIO mode ------- */
/*
 * Sets MCU_SEL=FUNC_GPIO, input enable, pull-up/down as specified.
 * FUN_DRV = 20mA default.
 *
 * Usage: IO_MUX_SET_GPIO(48, IO_MUX_FUN_WPU_M);   // GPIO48, pull-up
 */
#define IO_MUX_FUN_WPU_M   (1u << IO_MUX_FUN_WPU_S)
#define IO_MUX_FUN_WPD_M   (1u << IO_MUX_FUN_WPD_S)
#define IO_MUX_FUN_IE_M    (1u << IO_MUX_FUN_IE_S)
#define IO_MUX_MCU_SEL_M   (0x7u << IO_MUX_MCU_SEL_S)

/* Array-indexed access for GPIO 0-48 (gaps handled by bounds check in driver) */
extern const uint32_t io_mux_gpio_reg[49];

#endif /* ESP32S3_IO_MUX_REG_H */
