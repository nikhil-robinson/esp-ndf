/* =============================================================================
 * gpio_reg.h  –  ESP32-S3 GPIO controller register definitions
 *
 * The GPIO controller manages 45 digital I/O pins (GPIO0-21, GPIO26-48).
 * Pins GPIO22-25 do not exist on ESP32-S3 (they were present on ESP32).
 *
 * Base address: 0x60004000 (GPIO bank 0, GPIO0-31)
 * Extended:     0x60004074 (GPIO bank 1, GPIO32-48 via _1 registers)
 *
 * Source: ESP32-S3 TRM v1.4, Chapter 6 (GPIO and IOMUX)
 * ============================================================================= */
#ifndef ESP32S3_GPIO_REG_H
#define ESP32S3_GPIO_REG_H

#include "soc/soc.h"

/* ---- Register offsets from GPIO base (0x60004000) ----------------------- */

/* Output register (GPIO0-31): write 1 to set pin high, 0 for low */
#define GPIO_OUT_REG            (DR_REG_GPIO_BASE + 0x0004)
/* Output set: writing 1 to bit N sets GPIO N high (no read needed) */
#define GPIO_OUT_W1TS_REG       (DR_REG_GPIO_BASE + 0x0008)
/* Output clear: writing 1 to bit N drives GPIO N low */
#define GPIO_OUT_W1TC_REG       (DR_REG_GPIO_BASE + 0x000C)

/* Output register – GPIO32-48 (upper GPIO bank) */
#define GPIO_OUT1_REG           (DR_REG_GPIO_BASE + 0x0010)
#define GPIO_OUT1_W1TS_REG      (DR_REG_GPIO_BASE + 0x0014)
#define GPIO_OUT1_W1TC_REG      (DR_REG_GPIO_BASE + 0x0018)

/* Direction: 1 = output, 0 = input */
#define GPIO_ENABLE_REG         (DR_REG_GPIO_BASE + 0x0020)
#define GPIO_ENABLE_W1TS_REG    (DR_REG_GPIO_BASE + 0x0024)
#define GPIO_ENABLE_W1TC_REG    (DR_REG_GPIO_BASE + 0x0028)

/* Direction – GPIO32-48 */
#define GPIO_ENABLE1_REG        (DR_REG_GPIO_BASE + 0x002C)
#define GPIO_ENABLE1_W1TS_REG   (DR_REG_GPIO_BASE + 0x0030)
#define GPIO_ENABLE1_W1TC_REG   (DR_REG_GPIO_BASE + 0x0034)

/* Input (sampled): GPIO0-31 */
#define GPIO_IN_REG             (DR_REG_GPIO_BASE + 0x003C)
/* Input: GPIO32-48 */
#define GPIO_IN1_REG            (DR_REG_GPIO_BASE + 0x0040)

/* Interrupt status (GPIO0-31) */
#define GPIO_STATUS_REG         (DR_REG_GPIO_BASE + 0x0044)
#define GPIO_STATUS_W1TS_REG    (DR_REG_GPIO_BASE + 0x0048)
#define GPIO_STATUS_W1TC_REG    (DR_REG_GPIO_BASE + 0x004C)

/* Interrupt status (GPIO32-48) */
#define GPIO_STATUS1_REG        (DR_REG_GPIO_BASE + 0x0050)
#define GPIO_STATUS1_W1TS_REG   (DR_REG_GPIO_BASE + 0x0054)
#define GPIO_STATUS1_W1TC_REG   (DR_REG_GPIO_BASE + 0x0058)

/* NMI interrupt status (GPIO0-31) */
#define GPIO_PCPU_NMI_INT_ST    (DR_REG_GPIO_BASE + 0x005C)
#define GPIO_PCPU_NMI_INT_ST1   (DR_REG_GPIO_BASE + 0x0068)

/* ---- Per-pin configuration registers ------------------------------------ */
/*
 * GPIO_PINn_REG (n = 0..48):
 *   [0]    SYNC2_BYPASS     : use raw pad for input sync (bypass 2-FF stage)
 *   [2:1]  PAD_DRIVER       : 0 = normal, 1 = open-drain
 *   [4:3]  SYNC1_BYPASS     : similar to SYNC2
 *   [9:7]  INT_TYPE         : 0=disabled 1=pos-edge 2=neg-edge 3=any-edge
 *                             4=low-level 5=high-level
 *   [10]   WAKEUP_ENABLE    : enable wake from light-sleep on this GPIO
 *   [17:13] INT_ENA         : CPU0/CPU1 interrupt enable mask
 */
#define GPIO_PIN_REG(n)         (DR_REG_GPIO_BASE + 0x0074 + (n) * 4)
#define GPIO_PIN_PAD_DRIVER_S   2
#define GPIO_PIN_PAD_DRIVER_M   (0x1 << GPIO_PIN_PAD_DRIVER_S)
#define GPIO_PIN_INT_TYPE_S     7
#define GPIO_PIN_INT_TYPE_M     (0x7 << GPIO_PIN_INT_TYPE_S)
#define GPIO_PIN_WAKEUP_EN_S    10
#define GPIO_PIN_WAKEUP_EN_M    (0x1 << GPIO_PIN_WAKEUP_EN_S)
#define GPIO_PIN_INT_ENA_S      13
#define GPIO_PIN_INT_ENA_M      (0x1F << GPIO_PIN_INT_ENA_S)

/* GPIO_PIN_REG interrupt type values */
#define GPIO_INTR_DISABLE       0   /* No interrupt                           */
#define GPIO_INTR_POSEDGE       1   /* Rising edge                            */
#define GPIO_INTR_NEGEDGE       2   /* Falling edge                           */
#define GPIO_INTR_ANYEDGE       3   /* Both edges                             */
#define GPIO_INTR_LOW_LEVEL     4   /* Low level (hold)                       */
#define GPIO_INTR_HIGH_LEVEL    5   /* High level (hold)                      */

/* ---- GPIO function select (GPIO matrix) --------------------------------- */
/*
 * Each GPIO output can be driven by a peripheral signal (SIG_IN_SEL=0,
 * OEN_SEL=0) or forced to software control (OEN_SEL=1, SIG_OUT_SEL=256).
 *
 * GPIO_FUNCn_OUT_SEL_CFG_REG:
 *   [8:0]   OUT_SEL : signal number to route to GPIO output (256 = SW ctrl)
 *   [9]     OEN_INV : invert output enable
 *   [10]    OEN_SEL : override OEN (1=SW, 0=peripheral)
 *   [11]    OUT_INV : invert output
 *
 * GPIO_FUNCn_IN_SEL_CFG_REG  (in "GPIO signal" space, indexed by signal num):
 *   [5:0]   IN_SEL  : GPIO number to route to this function's input
 *   [6]     IN_INV  : invert input
 *   [7]     SIG_IN_SEL : 0=use pad directly, 1=this matrix entry
 */
#define GPIO_FUNC_OUT_SEL_CFG_REG(n) \
    (DR_REG_GPIO_BASE + 0x0554 + (n) * 4)
#define GPIO_OUT_SEL_S       0
#define GPIO_OUT_SEL_M       (0x1FF << GPIO_OUT_SEL_S)
#define GPIO_OUT_OEN_INV_S   9
#define GPIO_OUT_OEN_SEL_S   10
#define GPIO_OUT_OUT_INV_S   11
#define GPIO_OUT_SIG_SW_CTRL 0x100  /* Use SW control for output             */

/* GPIO matrix input: indexed by FUNCTION signal number, not GPIO number */
#define GPIO_FUNC_IN_SEL_CFG_REG(s) \
    (DR_REG_GPIO_BASE + 0x0154 + (s) * 4)
#define GPIO_IN_SEL_S        0
#define GPIO_IN_SEL_M        (0x3F << GPIO_IN_SEL_S)
#define GPIO_IN_INV_S        6
#define GPIO_SIG_IN_SEL_S    7
#define GPIO_SIG_IN_HIGH     0x3C   /* Force input to logic 1 */
#define GPIO_SIG_IN_LOW      0x3D   /* Force input to logic 0 */

/* ---- Handy bit-mask helpers --------------------------------------------- */
#define GPIO_PIN_MASk(n)   ((n) < 32 ? (1u << (n)) : (1u << ((n) - 32)))

#endif /* ESP32S3_GPIO_REG_H */
