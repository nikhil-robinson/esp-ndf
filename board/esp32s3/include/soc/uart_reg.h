/* =============================================================================
 * uart_reg.h  –  ESP32-S3 UART controller register definitions
 *
 * ESP32-S3 has 3 UART controllers (UART0, UART1, UART2):
 *   UART0: 0x60000000  (default console)
 *   UART1: 0x60010000
 *   UART2: 0x6002E000
 *
 * Features per UART:
 *  - 128-byte TX FIFO, 128-byte RX FIFO
 *  - Baud rate up to 5 Mbps
 *  - 8 programmable interrupt sources
 *  - Hardware flow control (RTS/CTS)
 *  - Infrared (IrDA) mode
 *  - RS-485 half-duplex support
 *
 * Source: ESP32-S3 TRM v1.4, Chapter 28 (UART Controller)
 * ============================================================================= */
#ifndef ESP32S3_UART_REG_H
#define ESP32S3_UART_REG_H

#include "soc/soc.h"

/* ---- UART register offsets (same for all three controllers) ------------- */

/* FIFO access: write TX byte, read RX byte */
#define UART_FIFO_REG                   0x0000
/* Inline read of current FIFO state */
#define UART_MEM_CONF_REG               0x0058

/* Interrupt raw status (W1C – write 1 to clear) */
#define UART_INT_RAW_REG                0x0004
/* Interrupt status (masked by UART_INT_ENA_REG) */
#define UART_INT_ST_REG                 0x0008
/* Interrupt enable mask */
#define UART_INT_ENA_REG                0x000C
/* Interrupt clear: write 1 to clear corresponding INT_RAW bit */
#define UART_INT_CLR_REG                0x0010

/* Clock divider (baud rate generator) */
#define UART_CLKDIV_REG                 0x0014
/* Auto baud rate detection */
#define UART_RX_FILT_REG                0x0018

/* Status register (TX/RX FIFO level, TX idle, etc.) */
#define UART_STATUS_REG                 0x001C
/* Configuration 0: data bits, stop bits, parity, bit order */
#define UART_CONF0_REG                  0x0020
/* Configuration 1: FIFO thresholds for interrupt generation */
#define UART_CONF1_REG                  0x0024
/* Low-pulse duration (auto baud) */
#define UART_LOWPULSE_REG               0x0028
/* High-pulse duration (auto baud) */
#define UART_HIGHPULSE_REG              0x002C
/* RXD edge interrupt counter */
#define UART_RXD_CNT_REG                0x0030

/* Flow control */
#define UART_FLOW_CONF_REG              0x0034
/* Sleep mode wake-up threshold */
#define UART_SLEEP_CONF_REG             0x0038
/* SW flow control (XON/XOFF) */
#define UART_SWFC_CONF0_SYNC_REG        0x003C
#define UART_SWFC_CONF1_REG             0x0040

/* RS-485 configuration */
#define UART_RS485_CONF_SYNC_REG        0x0044

/* AT command interrupt characters */
#define UART_AT_CMD_PRECNT_SYNC_REG     0x0048
#define UART_AT_CMD_POSTCNT_SYNC_REG    0x004C
#define UART_AT_CMD_GAPTOUT_SYNC_REG    0x0050
#define UART_AT_CMD_CHAR_SYNC_REG       0x0054

/* TX/RX FIFO depth configuration */
#define UART_MEM_CONF_REG               0x0058

/* Baud rate detection result */
#define UART_MEM_TX_STATUS_REG          0x005C
#define UART_MEM_RX_STATUS_REG          0x0060

/* ID / version */
#define UART_FSM_STATUS_REG             0x0064
#define UART_POSPULSE_REG               0x0068
#define UART_NEGPULSE_REG               0x006C
#define UART_CLK_CONF_REG               0x0070
#define UART_DATE_REG                   0x0074
#define UART_ID_REG                     0x0078

/* ---- Absolute register addresses (computed from base) ------------------- */
#define UART_FIFO(n)            (uart_base[(n)] + UART_FIFO_REG)
#define UART_INT_RAW(n)         (uart_base[(n)] + UART_INT_RAW_REG)
#define UART_INT_ST(n)          (uart_base[(n)] + UART_INT_ST_REG)
#define UART_INT_ENA(n)         (uart_base[(n)] + UART_INT_ENA_REG)
#define UART_INT_CLR(n)         (uart_base[(n)] + UART_INT_CLR_REG)
#define UART_CLKDIV(n)          (uart_base[(n)] + UART_CLKDIV_REG)
#define UART_STATUS(n)          (uart_base[(n)] + UART_STATUS_REG)
#define UART_CONF0(n)           (uart_base[(n)] + UART_CONF0_REG)
#define UART_CONF1(n)           (uart_base[(n)] + UART_CONF1_REG)
#define UART_CLK_CONF(n)        (uart_base[(n)] + UART_CLK_CONF_REG)

/* UART base address table (indexed by port number).
 * Declared extern here; defined once in board/esp32s3/board_init.c. */
extern const uint32_t uart_base[3];

/* ---- UART_CONF0_REG bit fields ------------------------------------------ */
#define UART_PARITY_S           0   /* [0]    Parity: 0=even, 1=odd          */
#define UART_PARITY_EN_S        1   /* [1]    Parity enable                  */
#define UART_BIT_NUM_S          2   /* [3:2]  Data bits: 0=5,1=6,2=7,3=8    */
#define UART_STOP_BIT_NUM_S     4   /* [5:4]  Stop bits: 0=1,1=1.5,2=2,3=3  */
#define UART_SW_RTS_S           6   /* [6]    SW RTS value                   */
#define UART_SW_DTR_S           7   /* [7]    SW DTR value                   */
#define UART_TXD_BRK_S          8   /* [8]    Send break (long low on TXD)   */
#define UART_IRDA_DPLX_S        9   /* [9]    IrDA duplex mode               */
#define UART_IRDA_TX_EN_S       10  /* [10]   IrDA TX enable                 */
#define UART_IRDA_WCTL_S        11  /* [11]   IrDA waveform inverse          */
#define UART_IRDA_TX_INV_S      12  /* [12]   IrDA TX inverse                */
#define UART_IRDA_RX_INV_S      13  /* [13]   IrDA RX inverse                */
#define UART_LOOPBACK_S         14  /* [14]   Loopback mode                  */
#define UART_TX_FLOW_EN_S       15  /* [15]   TX hardware flow control       */
#define UART_IRDA_EN_S          16  /* [16]   IrDA mode enable               */
#define UART_RXFIFO_RST_S       17  /* [17]   Reset RX FIFO                  */
#define UART_TXFIFO_RST_S       18  /* [18]   Reset TX FIFO                  */
#define UART_RXD_INV_S          19  /* [19]   Invert RXD input               */
#define UART_CTS_INV_S          20  /* [20]   Invert CTS input               */
#define UART_DSR_INV_S          21  /* [21]   Invert DSR input               */
#define UART_TXD_INV_S          22  /* [22]   Invert TXD output              */
#define UART_RTS_INV_S          23  /* [23]   Invert RTS output              */
#define UART_DTR_INV_S          24  /* [24]   Invert DTR output              */
#define UART_CLK_EN_S           25  /* [25]   Enable register clock          */
#define UART_ERR_WR_MASK_S      26  /* [26]   Mask errors in overrun state   */
#define UART_MEM_CLK_EN_S       28  /* [28]   FIFO memory clock enable       */

/* Data bit count field values */
#define UART_DATA_5_BITS        0
#define UART_DATA_6_BITS        1
#define UART_DATA_7_BITS        2
#define UART_DATA_8_BITS        3

/* Stop bit count field values */
#define UART_STOP_BITS_1        1
#define UART_STOP_BITS_1_5      2
#define UART_STOP_BITS_2        3

/* ---- UART_STATUS_REG bit fields ----------------------------------------- */
#define UART_RXFIFO_CNT_S       0   /* [7:0]  Bytes in RX FIFO              */
#define UART_RXFIFO_CNT_M       (0xFF << UART_RXFIFO_CNT_S)
#define UART_DSRN_S             13  /* [13]   DSR level                     */
#define UART_CTSN_S             14  /* [14]   CTS level                     */
#define UART_RXD_S              15  /* [15]   RXD level                     */
#define UART_TXFIFO_CNT_S       16  /* [23:16] Bytes in TX FIFO             */
#define UART_TXFIFO_CNT_M       (0xFF << UART_TXFIFO_CNT_S)
#define UART_DTRN_S             28  /* [28] DTR level                       */
#define UART_RTSN_S             29  /* [29] RTS level                       */
#define UART_TXD_S              30  /* [30] TXD level                       */

/* ---- UART interrupt bit fields (INT_RAW, INT_ST, INT_ENA, INT_CLR) ------ */
#define UART_RXFIFO_FULL_INT_S      0   /* RX FIFO reached threshold         */
#define UART_TXFIFO_EMPTY_INT_S     1   /* TX FIFO below threshold           */
#define UART_PARITY_ERR_INT_S       2   /* Parity error in received frame    */
#define UART_FRM_ERR_INT_S          3   /* Data frame error                  */
#define UART_RXFIFO_OVF_INT_S       4   /* RX FIFO overflow                  */
#define UART_DSR_CHG_INT_S          5   /* DSR changed                       */
#define UART_CTS_CHG_INT_S          6   /* CTS changed                       */
#define UART_BRK_DET_INT_S          7   /* Break detected on RXD             */
#define UART_RXFIFO_TOUT_INT_S      8   /* RX FIFO timeout (idle after data) */
#define UART_SW_XON_INT_S           9   /* SW flow XON received              */
#define UART_SW_XOFF_INT_S          10  /* SW flow XOFF received             */
#define UART_GLITCH_DET_INT_S       11  /* Glitch detection                  */
#define UART_TX_BRK_DONE_INT_S      12  /* TX break done                     */
#define UART_TX_BRK_IDLE_DONE_INT_S 13  /* TX break idle done                */
#define UART_TX_DONE_INT_S          14  /* TX complete (FIFO empty + idle)   */
#define UART_RS485_PARITY_ERR_INT_S 15
#define UART_RS485_FRM_ERR_INT_S    16
#define UART_RS485_CLASH_INT_S      17
#define UART_AT_CMD_CHAR_DET_INT_S  18

/* ---- UART_CLK_CONF_REG bit fields --------------------------------------- */
#define UART_CLK_DIV_FRAG_S         0   /* [3:0]  Fractional divider        */
#define UART_CLK_DIV_S              4   /* [27:4] Integer divider           */
#define UART_CLK_SEL_S              28  /* [29:28] Clock source:            */
                                        /*   0=disabled 1=APB 2=FOSC 3=XTAL */
#define UART_CLK_CONF_EN_S          31  /* [31]   Module clock enable (CLK_CONF reg) */
#define UART_CLK_APB                1
#define UART_CLK_FOSC               2   /* Internal 17.5 MHz RC oscillator  */
#define UART_CLK_XTAL               3   /* Crystal oscillator (40 MHz)      */

/* ---- UART FIFO size (ESP32-S3 UART has 128-byte TX and RX FIFOs) -------- */
#define UART_FIFO_LEN   128

#endif /* ESP32S3_UART_REG_H */
