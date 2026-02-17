/*
 * UART Hardware Abstraction Layer
 */

#ifndef HAL_UART_H
#define HAL_UART_H

#include <stdint.h>
#include <stdbool.h>

/* UART port numbers */
typedef enum {
    UART_NUM_0 = 0,
    UART_NUM_1 = 1,
    UART_NUM_MAX
} uart_port_t;

/* UART configuration */
typedef struct {
    uint32_t baud_rate;
    uint8_t data_bits;      /* 5, 6, 7, 8 */
    uint8_t stop_bits;      /* 1, 2 */
    uint8_t parity;         /* 0=none, 1=odd, 2=even */
} uart_config_t;

/* Initialize UART */
void uart_init(uart_port_t port, const uart_config_t *config);

/* Send a single byte */
void uart_putc(uart_port_t port, char c);

/* Send a string */
void uart_puts(uart_port_t port, const char *str);

/* Receive a single byte (blocking) */
char uart_getc(uart_port_t port);

/* Check if data available */
bool uart_available(uart_port_t port);

/* Flush TX buffer */
void uart_flush(uart_port_t port);

#endif /* HAL_UART_H */
