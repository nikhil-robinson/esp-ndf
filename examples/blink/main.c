/* =============================================================================
 * examples/blink/main.c  –  Bare-metal LED blink for ESP32-S3
 *
 * This is the first application to run on your bare-metal SDK.  It:
 *   1. Relies on board_init() (called before main by __start_c in start.S)
 *      to configure clocks at 240 MHz and UART0 at 115200 baud.
 *   2. Configures the built-in LED GPIO (GPIO48 on most ESP32-S3 DevKits).
 *   3. Blinks the LED at ~1 Hz while printing tick counts over UART0.
 *
 * Verified working on:
 *   - ESP32-S3-DevKitC-1 (LED on GPIO48)
 *   - Adjust BLINK_GPIO if your board uses a different pin.
 *
 * Expected UART output (115200 8N1):
 *   esp-ndf: bare-metal blink starting
 *   tick 0: LED ON
 *   tick 1: LED OFF
 *   tick 2: LED ON
 *   ...
 * ============================================================================= */
#include "board.h"          /* board_init, board_delay_ms, board_puts, etc.  */
#include "drivers/gpio/gpio.h"   /* gpio_set_direction, gpio_set_level, etc.      */
#include "drivers/uart/uart.h"   /* uart_printf                                   */

/* ---- Configuration ------------------------------------------------------- */
#define BLINK_GPIO      BOARD_LED_GPIO   /* GPIO48 on most ESP32-S3 DevKits  */
#define BLINK_PERIOD_MS 500              /* 500 ms on + 500 ms off = 1 Hz     */

/* ============================================================================
 * main
 * ============================================================================ */
int main(void)
{
    /* board_init() was already called by __start_c before main().
     * At this point:
     *   - CPU running at 240 MHz
     *   - UART0 configured at 115200 8N1 on GPIO43(TX)/GPIO44(RX)
     *   - Interrupts enabled
     */

    /* -- Announce startup -------------------------------------------------- */
    uart_printf(0, "\r\n");
    uart_printf(0, "========================================\r\n");
    uart_printf(0, "  esp-ndf bare-metal SDK\r\n");
    uart_printf(0, "  CPU: ESP32-S3 (Xtensa LX7 @ 240 MHz)\r\n");
    uart_printf(0, "  Blink example starting...\r\n");
    uart_printf(0, "========================================\r\n");
    uart_printf(0, "  LED GPIO: %d\r\n", BLINK_GPIO);
    uart_printf(0, "  Period:   %d ms\r\n", BLINK_PERIOD_MS * 2);
    uart_printf(0, "\r\n");

    /* -- Configure LED GPIO as push-pull output with default level LOW ----- */
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BLINK_GPIO, 0);

    /* -- Main loop --------------------------------------------------------- */
    uint32_t tick = 0;

    for (;;) {
        /* LED ON */
        gpio_set_level(BLINK_GPIO, 1);
        uart_printf(0, "tick %u: LED ON\r\n", tick++);
        board_delay_ms(BLINK_PERIOD_MS);

        /* LED OFF */
        gpio_set_level(BLINK_GPIO, 0);
        uart_printf(0, "tick %u: LED OFF\r\n", tick++);
        board_delay_ms(BLINK_PERIOD_MS);
    }

    /* Never reached, but the compiler likes a return for int main() */
    return 0;
}
