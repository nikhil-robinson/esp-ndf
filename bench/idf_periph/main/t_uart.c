#include "t.h"
#include "driver/uart.h"
#define LEN 4096
static uint8_t tx[LEN], rx[LEN];
static void run_baud(uint32_t baud)
{
    uart_config_t c = { .baud_rate = baud, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_APB };
    uart_param_config(1, &c);
    uint32_t actual; uart_get_baudrate(1, &actual);
    uart_flush_input(1);
    uint32_t s = 12345;
    for (int i = 0; i < LEN; i++) { s = s * 1103515245 + 12345; tx[i] = s >> 16; }
    uint32_t t0 = ccount();
    uart_write_bytes(1, tx, LEN);
    int n = 0;
    while (n < LEN && ccount() - t0 < 240000000) { int r = uart_read_bytes(1, rx + n, LEN - n, 1); if (r > 0) n += r; }
    uint32_t us = (ccount() - t0) / 240;
    int bad = 0;
    for (int i = 0; i < LEN; i++) bad += rx[i] != tx[i];
    printf("    %7lu baud (actual %lu): %d/%d bytes, %d wrong, %lu us (wire %lu us)\n",
           baud, actual, n, LEN, bad, us, (uint32_t)((uint64_t)LEN * 10 * 1000000 / baud));
}
void t_uart(void)
{
    uart_driver_install(1, 8192, 8192, 0, NULL, 0);
    uart_set_pin(1, 12, 13, -1, -1);
    run_baud(115200); run_baud(2000000); run_baud(5000000);
    uint8_t m[16] = "0123456789abcdef";
    METRIC("uart_write() 16 bytes", "%lu cycles", CYCLES(uart_write_bytes(1, m, 16)));
}
