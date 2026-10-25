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

/* UHCI DMA, 4 KB each way at 5 Mbaud over the same jumper; CPU time = cycles inside the driver calls */
static void dma(void)
{
    uart_cfg_t c = { .baud = 5000000, .tx_pin = 12, .rx_pin = 13 };
    uart_init(1, &c);
    CHECK(uart_dma_start(1, 0) == 0);
    for (int i = 0; i < LEN; i++) { tx[i] = i * 7 + 3; rx[i] = 0; }
    uint32_t c0 = ccount();
    CHECK(uart_dma_read_start(1, rx, LEN) == 0);
    uint32_t c1 = ccount();
    CHECK(uart_dma_write(1, tx, LEN, 0) == LEN);             /* async: the CPU is free from here */
    uint32_t c2 = ccount();
    int n; while (((n = uart_dma_read_done(1)) < 0 || !uart_dma_write_done(1)) && ccount() - c0 < 240000000u) {}
    uint32_t us = (ccount() - c0) / 240;
    int bad = 0; for (int i = 0; i < LEN; i++) bad += rx[i] != tx[i];
    uart_dma_stop(1);
    printf("    DMA 5 Mbaud: %d/%d bytes, %d wrong, %lu us (wire %u us), CPU in calls %lu + %lu cycles\n",
           n, LEN, bad, us, LEN * 10 / 5, c1 - c0, c2 - c1);
    CHECK(n == LEN && bad == 0);
}

void test_uart(void)
{
    __asm__ volatile("rsil a2, 0" ::: "a2");
    run_baud(115200);
    run_baud(2000000);
    run_baud(5000000);
    dma();
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

NDF_UNIT(010, uart);
