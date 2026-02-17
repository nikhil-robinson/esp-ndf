/*
 * ESP32-S3 UART Register Definitions
 */

#ifndef REG_UART_REG_H
#define REG_UART_REG_H

#include "../arch/esp32s3/esp32s3.h"

/* UART0 Registers */
#define UART_FIFO_REG(i)            (DR_REG_UART0_BASE + (i) * 0x10000 + 0x0000)
#define UART_INT_RAW_REG(i)         (DR_REG_UART0_BASE + (i) * 0x10000 + 0x0004)
#define UART_INT_ST_REG(i)          (DR_REG_UART0_BASE + (i) * 0x10000 + 0x0008)
#define UART_INT_ENA_REG(i)         (DR_REG_UART0_BASE + (i) * 0x10000 + 0x000C)
#define UART_INT_CLR_REG(i)         (DR_REG_UART0_BASE + (i) * 0x10000 + 0x0010)
#define UART_CLKDIV_REG(i)          (DR_REG_UART0_BASE + (i) * 0x10000 + 0x0014)
#define UART_RX_FILT_REG(i)         (DR_REG_UART0_BASE + (i) * 0x10000 + 0x0018)
#define UART_STATUS_REG(i)          (DR_REG_UART0_BASE + (i) * 0x10000 + 0x001C)
#define UART_CONF0_REG(i)           (DR_REG_UART0_BASE + (i) * 0x10000 + 0x0020)
#define UART_CONF1_REG(i)           (DR_REG_UART0_BASE + (i) * 0x10000 + 0x0024)

/* UART_CONF0 bits */
#define UART_PARITY_EN              (1 << 0)
#define UART_PARITY                 (1 << 1)    /* 0=even, 1=odd */
#define UART_BIT_NUM_S              2
#define UART_BIT_NUM_M              (0x3 << UART_BIT_NUM_S)
#define UART_STOP_BIT_NUM_S         4
#define UART_STOP_BIT_NUM_M         (0x3 << UART_STOP_BIT_NUM_S)
#define UART_TXD_BRK                (1 << 8)
#define UART_LOOPBACK               (1 << 14)
#define UART_TX_FLOW_EN             (1 << 15)
#define UART_RXFIFO_RST             (1 << 17)
#define UART_TXFIFO_RST             (1 << 18)

/* UART_STATUS bits */
#define UART_RXFIFO_CNT_S           0
#define UART_RXFIFO_CNT_M           (0x3FF << UART_RXFIFO_CNT_S)
#define UART_TXFIFO_CNT_S           16
#define UART_TXFIFO_CNT_M           (0x3FF << UART_TXFIFO_CNT_S)

/* Data bits */
#define UART_DATA_5_BITS            0
#define UART_DATA_6_BITS            1
#define UART_DATA_7_BITS            2
#define UART_DATA_8_BITS            3

/* Stop bits */
#define UART_STOP_1_BIT             1
#define UART_STOP_1_5_BITS          2
#define UART_STOP_2_BITS            3

/* Clock divider for baud rate */
#define UART_CLK_FREQ               40000000    /* APB clock 40MHz */

#endif /* REG_UART_REG_H */
