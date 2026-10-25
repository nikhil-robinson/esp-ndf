/* IDF sibling of periph_test uart dma(): UHCI DMA, UART1 5 Mbaud, TX 12 -> jumper -> RX 13, 4 KB each way. */
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/uhci.h"
#include "esp_cpu.h"

#define LEN 4096
static uint8_t tx[LEN], rx[LEN];
static volatile int got = -1;

static bool on_rx(uhci_controller_handle_t h, const uhci_rx_event_data_t *e, void *ctx)
{
    static int acc;
    if (e->data != rx + acc) memcpy(rx + acc, e->data, e->recv_size);   /* data may sit in the driver's buffer */
    acc += e->recv_size;
    if (e->flags.totally_received) { got = acc; acc = 0; }
    return false;
}

void app_main(void)
{
    uart_config_t uc = { .baud_rate = 5000000, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                         .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_APB };
    uart_param_config(UART_NUM_1, &uc);
    uart_set_pin(UART_NUM_1, 12, 13, -1, -1);
    uhci_controller_config_t cfg = { .uart_port = UART_NUM_1, .tx_trans_queue_depth = 4, .max_receive_internal_mem = 10 * 1024,
                                     .max_transmit_size = LEN, .dma_burst_size = 32, .rx_eof_flags.idle_eof = 1 };
    uhci_controller_handle_t h;
    printf("new %d\n", uhci_new_controller(&cfg, &h));
    uhci_event_callbacks_t cb = { .on_rx_trans_event = on_rx };
    uhci_register_event_callbacks(h, &cb, NULL);
    for (int i = 0; i < LEN; i++) tx[i] = i * 7 + 3;
    uint32_t c0 = esp_cpu_get_cycle_count();
    uhci_receive(h, rx, LEN);
    uint32_t c1 = esp_cpu_get_cycle_count();
    uhci_transmit(h, tx, LEN);
    uint32_t c2 = esp_cpu_get_cycle_count();
    while (got < 0 && esp_cpu_get_cycle_count() - c0 < 240000000u) {}
    uint32_t us = (esp_cpu_get_cycle_count() - c0) / 240;
    int bad = 0; for (int i = 0; i < LEN; i++) bad += rx[i] != tx[i];
    printf("DMA 5 Mbaud: %d/%d bytes, %d wrong, %lu us (wire 8192 us), CPU in calls %lu + %lu cycles\n",
           got, LEN, bad, us, c1 - c0, c2 - c1);
    printf("idf_uart_dma done\n");
}
