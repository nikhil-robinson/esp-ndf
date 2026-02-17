/*
 * ESP32-S3 USB Serial/JTAG Controller Register Definitions
 * Reference: ESP32-S3 Technical Reference Manual
 * 
 * The USB Serial/JTAG controller provides:
 * - USB CDC for serial console (debug output)
 * - USB JTAG for hardware debugging
 */

#ifndef REG_USB_SERIAL_JTAG_REG_H
#define REG_USB_SERIAL_JTAG_REG_H

#include "../arch/esp32s3/esp32s3.h"

/* USB Serial/JTAG base address */
#define DR_REG_USB_SERIAL_JTAG_BASE     0x60038000

/* EP1 - Data endpoint for CDC serial */
#define USB_SERIAL_JTAG_EP1_REG         (DR_REG_USB_SERIAL_JTAG_BASE + 0x0000)
#define USB_SERIAL_JTAG_EP1_CONF_REG    (DR_REG_USB_SERIAL_JTAG_BASE + 0x0004)

/* EP1_CONF register bits */
#define USB_SERIAL_JTAG_WR_DONE         (1 << 0)    /* Set when write to EP1 is done */
#define USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE (1 << 1) /* EP1 buffer is free for write */
#define USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL (1 << 2) /* Data available to read */

/* Configuration registers */
#define USB_SERIAL_JTAG_CONF0_REG       (DR_REG_USB_SERIAL_JTAG_BASE + 0x0018)
#define USB_SERIAL_JTAG_TEST_REG        (DR_REG_USB_SERIAL_JTAG_BASE + 0x001C)
#define USB_SERIAL_JTAG_MISC_CONF_REG   (DR_REG_USB_SERIAL_JTAG_BASE + 0x0044)

/* CONF0 register bits */
#define USB_SERIAL_JTAG_PHY_SEL         (1 << 0)    /* 0=internal PHY, 1=external */
#define USB_SERIAL_JTAG_EXCHG_PINS_OVERRIDE (1 << 1)
#define USB_SERIAL_JTAG_EXCHG_PINS      (1 << 2)
#define USB_SERIAL_JTAG_VREFL_S         3
#define USB_SERIAL_JTAG_VREFL_M         (0x3 << 3)
#define USB_SERIAL_JTAG_VREFH_S         5
#define USB_SERIAL_JTAG_VREFH_M         (0x3 << 5)
#define USB_SERIAL_JTAG_VREF_OVERRIDE   (1 << 7)
#define USB_SERIAL_JTAG_PAD_PULL_OVERRIDE (1 << 8)
#define USB_SERIAL_JTAG_DP_PULLUP       (1 << 9)
#define USB_SERIAL_JTAG_DP_PULLDOWN     (1 << 10)
#define USB_SERIAL_JTAG_DM_PULLUP       (1 << 11)
#define USB_SERIAL_JTAG_DM_PULLDOWN     (1 << 12)
#define USB_SERIAL_JTAG_PULLUP_VALUE    (1 << 13)
#define USB_SERIAL_JTAG_USB_PAD_ENABLE  (1 << 14)

/* Interrupt registers */
#define USB_SERIAL_JTAG_INT_RAW_REG     (DR_REG_USB_SERIAL_JTAG_BASE + 0x0008)
#define USB_SERIAL_JTAG_INT_ST_REG      (DR_REG_USB_SERIAL_JTAG_BASE + 0x000C)
#define USB_SERIAL_JTAG_INT_ENA_REG     (DR_REG_USB_SERIAL_JTAG_BASE + 0x0010)
#define USB_SERIAL_JTAG_INT_CLR_REG     (DR_REG_USB_SERIAL_JTAG_BASE + 0x0014)

/* Interrupt bits */
#define USB_SERIAL_JTAG_JTAG_IN_FLUSH_INT       (1 << 0)
#define USB_SERIAL_JTAG_SOF_INT                 (1 << 1)
#define USB_SERIAL_JTAG_SERIAL_OUT_RECV_PKT_INT (1 << 2)
#define USB_SERIAL_JTAG_SERIAL_IN_EMPTY_INT     (1 << 3)
#define USB_SERIAL_JTAG_PID_ERR_INT             (1 << 4)
#define USB_SERIAL_JTAG_CRC5_ERR_INT            (1 << 5)
#define USB_SERIAL_JTAG_CRC16_ERR_INT           (1 << 6)
#define USB_SERIAL_JTAG_STUFF_ERR_INT           (1 << 7)
#define USB_SERIAL_JTAG_IN_TOKEN_REC_IN_EP1_INT (1 << 8)
#define USB_SERIAL_JTAG_USB_BUS_RESET_INT       (1 << 9)
#define USB_SERIAL_JTAG_OUT_EP1_ZERO_PAYLOAD_INT (1 << 10)
#define USB_SERIAL_JTAG_OUT_EP2_ZERO_PAYLOAD_INT (1 << 11)

/* OUT EP1 registers (for reading data from host) */
#define USB_SERIAL_JTAG_OUT_EP1_REG     (DR_REG_USB_SERIAL_JTAG_BASE + 0x003C)
#define USB_SERIAL_JTAG_OUT_EP1_ST_REG  (DR_REG_USB_SERIAL_JTAG_BASE + 0x0040)

/* OUT_EP1_ST bits */
#define USB_SERIAL_JTAG_OUT_EP1_STATE_S     0
#define USB_SERIAL_JTAG_OUT_EP1_STATE_M     (0x3 << 0)
#define USB_SERIAL_JTAG_OUT_EP1_WR_ADDR_S   2
#define USB_SERIAL_JTAG_OUT_EP1_WR_ADDR_M   (0x7F << 2)
#define USB_SERIAL_JTAG_OUT_EP1_RD_ADDR_S   9
#define USB_SERIAL_JTAG_OUT_EP1_RD_ADDR_M   (0x7F << 9)
#define USB_SERIAL_JTAG_OUT_EP1_REC_DATA_CNT_S 16
#define USB_SERIAL_JTAG_OUT_EP1_REC_DATA_CNT_M (0x7F << 16)

/* IN EP1 state register */
#define USB_SERIAL_JTAG_IN_EP1_ST_REG   (DR_REG_USB_SERIAL_JTAG_BASE + 0x002C)

/* IN_EP1_ST bits */
#define USB_SERIAL_JTAG_IN_EP1_STATE_S      0
#define USB_SERIAL_JTAG_IN_EP1_STATE_M      (0x3 << 0)
#define USB_SERIAL_JTAG_IN_EP1_WR_ADDR_S    2
#define USB_SERIAL_JTAG_IN_EP1_WR_ADDR_M    (0x7F << 2)
#define USB_SERIAL_JTAG_IN_EP1_RD_ADDR_S    9
#define USB_SERIAL_JTAG_IN_EP1_RD_ADDR_M    (0x7F << 9)

/* Endpoint buffer size */
#define USB_SERIAL_JTAG_EP1_BUF_SIZE    64

#endif /* REG_USB_SERIAL_JTAG_REG_H */
