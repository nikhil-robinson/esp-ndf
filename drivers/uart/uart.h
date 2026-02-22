/* =============================================================================
 * uart.h  –  ESP32-S3 UART driver API
 *
 * Supports three UART controllers (port 0, 1, 2).
 * Provides:
 *  - Blocking (polled) TX/RX  — useful for debug output
 *  - Interrupt-driven TX/RX with ring buffers (optional)
 *  - Printf-style formatted output
 *
 * Usage (blocking mode):
 *   uart_config_t cfg = {
 *       .baud_rate = 115200,
 *       .data_bits = UART_DATA_8_BITS,
 *       .stop_bits = UART_STOP_BITS_1,
 *       .parity    = UART_NO_PARITY,
 *   };
 *   uart_init(0, &cfg);
 *   uart_puts(0, "Hello, bare-metal!\r\n");
 * ============================================================================= */
#ifndef NDF_UART_H
#define NDF_UART_H

#include "types.h"
#include "soc/uart_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Parity modes -------------------------------------------------------- */
typedef enum {
    UART_NO_PARITY  = 0,
    UART_EVEN_PARITY = 1,
    UART_ODD_PARITY  = 3,
} uart_parity_t;

/* ---- Hardware flow control modes ---------------------------------------- */
typedef enum {
    UART_HW_FLOWCTRL_DISABLE = 0,
    UART_HW_FLOWCTRL_RTS     = 1,
    UART_HW_FLOWCTRL_CTS     = 2,
    UART_HW_FLOWCTRL_CTS_RTS = 3,
} uart_hw_flowctrl_t;

/* ---- UART configuration structure --------------------------------------- */
typedef struct {
    uint32_t            baud_rate;  /* Baud rate (e.g. 115200)              */
    uint8_t             data_bits;  /* UART_DATA_5_BITS … UART_DATA_8_BITS  */
    uint8_t             stop_bits;  /* UART_STOP_BITS_1, _1_5, _2           */
    uart_parity_t       parity;     /* UART_NO_PARITY, _EVEN, _ODD          */
    uart_hw_flowctrl_t  flow_ctrl;  /* Hardware flow control                */
} uart_config_t;

/* ---- Default configurations (convenience) ------------------------------- */
#define UART_CONFIG_DEFAULT() {             \
    .baud_rate = 115200,                    \
    .data_bits = UART_DATA_8_BITS,          \
    .stop_bits = UART_STOP_BITS_1,          \
    .parity    = UART_NO_PARITY,            \
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,  \
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * @brief  Initialise a UART controller.
 *
 * Configures baud rate, frame format, and GPIO pins.
 * Default GPIO assignments for each port:
 *   UART0: TXD=GPIO43, RXD=GPIO44  (matches most ESP32-S3 DevKit boards)
 *   UART1: TXD=GPIO17, RXD=GPIO18  (example assignment, adjust as needed)
 *   UART2: TXD=GPIO19, RXD=GPIO20
 *
 * @param port  UART port number (0, 1, or 2)
 * @param cfg   Pointer to configuration structure
 * @return 0 on success, -1 on invalid port
 */
int uart_init(int port, const uart_config_t *cfg);

/**
 * @brief  Write a single byte to the TX FIFO (blocking – waits for space).
 */
void uart_putc(int port, uint8_t c);

/**
 * @brief  Read a single byte from the RX FIFO (blocking – waits for data).
 */
uint8_t uart_getc(int port);

/**
 * @brief  Write a null-terminated string (blocking).
 */
void uart_puts(int port, const char *s);

/**
 * @brief  Read up to n bytes into buf (blocking read of exactly n bytes).
 */
void uart_read(int port, uint8_t *buf, size_t n);

/**
 * @brief  Return the number of bytes available in the RX FIFO.
 */
int uart_rx_available(int port);

/**
 * @brief  Return non-zero if the TX FIFO is empty and the transmitter is idle.
 */
int uart_tx_idle(int port);

/**
 * @brief  Flush the TX FIFO (wait until all bytes are transmitted).
 */
void uart_flush_tx(int port);

/**
 * @brief  Minimal printf-like formatted output to a UART port.
 *
 * Supported format specifiers:
 *   %d / %i  – signed decimal integer
 *   %u       – unsigned decimal integer
 *   %x / %X  – hexadecimal (lower/upper)
 *   %s       – null-terminated string
 *   %c       – single character
 *   %%       – literal percent
 *   %08x     – zero-padded 8-digit hex (width specifier supported)
 *
 * NOTE: No floating-point support (no stdlib dependency).
 */
int uart_printf(int port, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* NDF_UART_H */
