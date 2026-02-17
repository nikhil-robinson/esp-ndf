/*
 * UART Hardware Abstraction Layer Implementation
 */

#include "uart.h"
#include "../reg/uart_reg.h"

void uart_init(uart_port_t port, const uart_config_t *config)
{
    if (port >= UART_NUM_MAX) return;
    
    /* Calculate clock divider */
    uint32_t clk_div = UART_CLK_FREQ / config->baud_rate;
    REG_WRITE(UART_CLKDIV_REG(port), clk_div);
    
    /* Configure UART: data bits, stop bits, parity */
    uint32_t conf0 = 0;
    
    /* Data bits (5-8) */
    uint8_t data_bits = config->data_bits;
    if (data_bits < 5) data_bits = 5;
    if (data_bits > 8) data_bits = 8;
    conf0 |= ((data_bits - 5) << UART_BIT_NUM_S);
    
    /* Stop bits */
    if (config->stop_bits == 2) {
        conf0 |= (UART_STOP_2_BITS << UART_STOP_BIT_NUM_S);
    } else {
        conf0 |= (UART_STOP_1_BIT << UART_STOP_BIT_NUM_S);
    }
    
    /* Parity */
    if (config->parity > 0) {
        conf0 |= UART_PARITY_EN;
        if (config->parity == 1) {  /* Odd */
            conf0 |= UART_PARITY;
        }
    }
    
    REG_WRITE(UART_CONF0_REG(port), conf0);
    
    /* Reset FIFOs */
    REG_SET_BIT(UART_CONF0_REG(port), UART_RXFIFO_RST | UART_TXFIFO_RST);
    REG_CLR_BIT(UART_CONF0_REG(port), UART_RXFIFO_RST | UART_TXFIFO_RST);
}

void uart_putc(uart_port_t port, char c)
{
    if (port >= UART_NUM_MAX) return;
    
    /* Wait for TX FIFO space */
    while (((REG_READ(UART_STATUS_REG(port)) >> UART_TXFIFO_CNT_S) & 0x3FF) >= 128) {
        /* Wait */
    }
    
    REG_WRITE(UART_FIFO_REG(port), c);
}

void uart_puts(uart_port_t port, const char *str)
{
    while (*str) {
        uart_putc(port, *str++);
    }
}

char uart_getc(uart_port_t port)
{
    if (port >= UART_NUM_MAX) return 0;
    
    /* Wait for data */
    while (!uart_available(port)) {
        /* Wait */
    }
    
    return REG_READ(UART_FIFO_REG(port)) & 0xFF;
}

bool uart_available(uart_port_t port)
{
    if (port >= UART_NUM_MAX) return false;
    
    return ((REG_READ(UART_STATUS_REG(port)) >> UART_RXFIFO_CNT_S) & 0x3FF) > 0;
}

void uart_flush(uart_port_t port)
{
    if (port >= UART_NUM_MAX) return;
    
    /* Wait for TX FIFO empty */
    while (((REG_READ(UART_STATUS_REG(port)) >> UART_TXFIFO_CNT_S) & 0x3FF) > 0) {
        /* Wait */
    }
}
