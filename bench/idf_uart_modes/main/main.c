/* IDF sibling of examples/periph_test/test_uart_modes.c: UART1 TX 12 -> RX 13, RTS/DE pad 16. */
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_cpu.h"
#include "driver/gpio.h"
#include "soc/io_mux_reg.h"
#include "hal/gpio_types.h"
#include "soc/gpio_periph.h"
#include "soc/gpio_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint8_t tx[512], rx[512];

void app_main(void)
{
    uart_config_t c = { .baud_rate = 1000000, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_APB };
    uart_driver_install(UART_NUM_1, 1024, 1024, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &c);
    uart_set_pin(UART_NUM_1, 12, 13, 16, UART_PIN_NO_CHANGE);
    uart_set_mode(UART_NUM_1, UART_MODE_RS485_HALF_DUPLEX);
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[12]); PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[16]);             /* read the pads back */
    for (int i = 0; i < 32; i++) tx[i] = 0xA0 + i;
    uint32_t best = ~0u;
    for (int k = 0; k < 5; k++) {
        vTaskDelay(2);
        uart_write_bytes(UART_NUM_1, tx, 32);
        uint32_t last = esp_cpu_get_cycle_count(), t0 = last, tde = 0, prev = REG_READ(GPIO_IN_REG);
        while (esp_cpu_get_cycle_count() - t0 < 240000u * 2) {
            uint32_t v = REG_READ(GPIO_IN_REG), now = esp_cpu_get_cycle_count();
            if ((v ^ prev) & BIT(12)) last = now;
            if ((v ^ prev) & BIT(16)) { if (k == 0) printf("  DE -> %d at %lu (last tx edge %lu)\n", (int)((v >> 16) & 1), now - t0, last - t0); if (!(v & BIT(16)) && !tde) tde = now; }
            prev = v;
        }
        if (tde && tde - last < best) best = tde - last;
    }
    int echo = uart_read_bytes(UART_NUM_1, rx, 32, 2);
    printf("RS485: DE low %lu cycles after the last edge (stop bit 240) -> %lu after stop, echo %d\n", best, best - 240, echo);

    uart_set_mode(UART_NUM_1, UART_MODE_UART);
    c.baud_rate = 115200;
    uart_param_config(UART_NUM_1, &c);
    uart_enable_pattern_det_baud_intr(UART_NUM_1, '+', 3, 9, 0, 0);
    uart_pattern_queue_reset(UART_NUM_1, 8);
    uart_flush_input(UART_NUM_1);
    uart_write_bytes(UART_NUM_1, "hello+++world", 13);
    vTaskDelay(5);
    printf("pattern at %d\n", uart_pattern_pop_pos(UART_NUM_1));
    printf("idf_uart_modes done\n");
}
