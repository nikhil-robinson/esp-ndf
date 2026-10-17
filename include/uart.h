/* UART 0/1/2: fractional baud (APB 80 MHz), 128-byte HW FIFOs, IRQ-driven
 * RX and TX ring buffers. No byte is lost while the application is busy as
 * long as the RX ring has room (overflow is counted, never silent). */
#pragma once
#include "ndf.h"

typedef enum { UART_PARITY_NONE = 0, UART_PARITY_EVEN = 2, UART_PARITY_ODD = 3 } uart_parity_t;

typedef struct {
    uint32_t baud;
    uint8_t data_bits;       /* 5..8, 0 = 8 */
    uint8_t stop_bits;       /* 1 or 2, 0 = 1 */
    uart_parity_t parity;
    int tx_pin, rx_pin;      /* -1: unused */
} uart_cfg_t;

/* Returns the achieved baud rate (from the real divider), 0 on error. */
uint32_t uart_init(int port, const uart_cfg_t *cfg);
int      uart_write(int port, const void *buf, size_t n);   /* blocks until queued */
int      uart_read(int port, void *buf, size_t n);          /* non-blocking, returns bytes */
size_t   uart_available(int port);
void     uart_flush(int port);                               /* wait until all sent on the wire */
uint32_t uart_overflows(int port);                           /* RX bytes dropped (ring full) */
