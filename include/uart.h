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

/* Hardware flow control: RTS raised when the RX FIFO holds rx_thresh bytes (0 = 100), TX paused while CTS is high.
 * Pins < 0: that side off. */
int      uart_set_flow(int port, int rts_pin, int cts_pin, uint32_t rx_thresh);
/* Modes (IDF uart_ll_set_mode_*): RS485 = half duplex, DE on de_pin (RTS signal) high while sending, echo dropped;
 * RS485_CD = collision detect (TX looped to RX, clash / frame / parity errors counted, RX FIFO reset);
 * IRDA = IrDA SIR pulses. */
enum { UART_MODE_UART = 0, UART_MODE_RS485, UART_MODE_RS485_CD, UART_MODE_IRDA };
int      uart_set_mode(int port, int mode, int de_pin);
uint32_t uart_rs485_clashes(int port);
/* Pattern (AT command) detection (n = 0: off): n x char c, gaps <= gap, idle >= pre before and >= post after (baud cycles).
 * uart_pattern_pop: offset of the next pattern from the next byte uart_read() returns, -1 none (queue of 8). */
int      uart_set_pattern(int port, char c, int n, uint32_t gap, uint32_t pre, uint32_t post);
int      uart_pattern_pop(int port);
