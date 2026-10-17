#include "test.h"
#include "uart.h"
#include "sys.h"

/* UART1: TX GPIO12 -> jumper -> RX GPIO13 (docs/WIRING.md) */
#define LEN 4096
static uint8_t tx[LEN], rx[LEN];

static void run_baud(uint32_t baud)
{
    uart_cfg_t c = { .baud = baud, .tx_pin = 12, .rx_pin = 13 };
    uint32_t got = uart_init(1, &c);
    uint32_t s = 12345;
    for (int i = 0; i < LEN; i++) { s = s * 1103515245 + 12345; tx[i] = s >> 16; }
    uint32_t t0 = ccount();
    uart_write(1, tx, LEN);
    size_t n = 0;
    while (n < LEN && ccount() - t0 < 240000000) n += uart_read(1, rx + n, LEN - n);
    uint32_t us = (ccount() - t0) / 240;
    int bad = 0;
    for (int i = 0; i < LEN; i++) bad += rx[i] != tx[i];
    printf("    %7lu baud (actual %lu): %u/%d bytes, %d wrong, %lu us (wire %lu us), ovf %lu\n",
           baud, got, n, LEN, bad, us, (uint32_t)((uint64_t)LEN * 10 * 1000000 / baud), uart_overflows(1));
    CHECK(n == LEN && bad == 0 && uart_overflows(1) == 0);
}

void test_uart(void)
{
    __asm__ volatile("rsil a2, 0" ::: "a2");
    run_baud(115200);
    run_baud(2000000);
    run_baud(5000000);
    uint8_t m[16] = "0123456789abcdef";
    METRIC("uart_write() 16 bytes", "%lu cycles", CYCLES(uart_write(1, m, 16)));
    uart_flush(1);

    /* baud accuracy, for the analyzer: UART2 TX on GPIO4 (analyzer D1), 0x55 = 0101.. */
    uart_cfg_t c2 = { .baud = 115200, .tx_pin = 4, .rx_pin = -1 };
    uart_init(2, &c2);
    printf("    analyzer: UART2 0x55 @115200 on GPIO4\n");
    static uint8_t u55[1000];
    for (int i = 0; i < 1000; i++) u55[i] = 0x55;
    uart_write(2, u55, 1000);
    uart_flush(2);
}
