/* =============================================================================
 * board.h  –  ESP32-S3 board top-level include
 *
 * Include this single header to pull in:
 *   - Common types (types.h)
 *   - Compiler attributes (compiler.h)
 *   - All ESP32-S3 SoC register definitions
 *   - Xtensa special-register macros
 *   - Interrupt management API
 *
 * Board: ESP32-S3 (Xtensa LX7 dual-core, up to 240MHz)
 * ============================================================================= */
#ifndef NDF_BOARD_ESP32S3_H
#define NDF_BOARD_ESP32S3_H

/* ---- Common SDK headers ------------------------------------------------- */
#include "types.h"
#include "compiler.h"

/* ---- ESP32-S3 SoC register maps ----------------------------------------- */
#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include "soc/uart_reg.h"
#include "soc/system_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/io_mux_reg.h"

/* ---- Xtensa architecture headers ---------------------------------------- */
#include "xtensa_ops.h"
#include "intr.h"

/* ---- Board configuration ------------------------------------------------ */

/** CPU frequency after board_init() completes */
#define BOARD_CPU_FREQ_HZ       240000000UL   /* 240 MHz                   */
#define BOARD_APB_FREQ_HZ        80000000UL   /* APB always 80 MHz with PLL */
#define BOARD_XTAL_FREQ_HZ       40000000UL   /* External crystal           */

/** Default UART port used for early debug prints */
#define BOARD_DEBUG_UART_PORT   0
#define BOARD_DEBUG_UART_BAUD   115200

/** Built-in LED GPIO (common on many ESP32-S3 DevKit boards = GPIO48) */
#define BOARD_LED_GPIO          48

/** UART0 default pin assignment (IO MUX direct mode) */
#define BOARD_UART0_TXD_GPIO    43
#define BOARD_UART0_RXD_GPIO    44

/* ---- Public API ---------------------------------------------------------- */

/**
 * @brief  Initialise the ESP32-S3 hardware.
 *
 * Called by __start_c() before main(). Performs in strict order:
 *   1. Disable all WDTs (TIMG0, TIMG1, RTC)
 *   2. Disable brown-out detector during PLL lock
 *   3. Configure PLL: XTAL → 480 MHz PLL → CPU 240 MHz, APB 80 MHz
 *   4. Re-enable brown-out detector
 *   5. Enable clocks for GPIO, UART, SYSTIMER
 *   6. Initialise interrupt controller
 *   7. Configure UART0 at BOARD_DEBUG_UART_BAUD for debug output
 */
void board_init(void);

/**
 * @brief  Write one character to the debug UART (blocking, polls TX-empty).
 */
void board_putc(char c);

/**
 * @brief  Write a null-terminated string to the debug UART.
 */
void board_puts(const char *s);

/**
 * @brief  Busy-wait for approximately 'us' microseconds.
 * Accuracy depends on CPU frequency; uses the Xtensa CCOUNT register.
 */
void board_delay_us(uint32_t us);

/**
 * @brief  Busy-wait for approximately 'ms' milliseconds.
 */
void board_delay_ms(uint32_t ms);

/**
 * @brief  Return the current CCOUNT (cycle counter) tick.
 * Use two readings & subtract to measure elapsed cycles.
 */
static inline uint32_t board_ccount(void)
{
    return xtensa_ccount();
}

#endif /* NDF_BOARD_ESP32S3_H */
