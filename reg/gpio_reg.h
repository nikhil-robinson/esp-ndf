/*
 * ESP32-S3 GPIO Register Definitions
 * Raw register addresses and bit definitions
 */

#ifndef REG_GPIO_REG_H
#define REG_GPIO_REG_H

#include "../arch/esp32s3/esp32s3.h"

/* GPIO Output Registers */
#define GPIO_OUT_REG                (DR_REG_GPIO_BASE + 0x0004)
#define GPIO_OUT_W1TS_REG           (DR_REG_GPIO_BASE + 0x0008)
#define GPIO_OUT_W1TC_REG           (DR_REG_GPIO_BASE + 0x000C)
#define GPIO_OUT1_REG               (DR_REG_GPIO_BASE + 0x0010)
#define GPIO_OUT1_W1TS_REG          (DR_REG_GPIO_BASE + 0x0014)
#define GPIO_OUT1_W1TC_REG          (DR_REG_GPIO_BASE + 0x0018)

/* GPIO Output Enable Registers */
#define GPIO_ENABLE_REG             (DR_REG_GPIO_BASE + 0x0020)
#define GPIO_ENABLE_W1TS_REG        (DR_REG_GPIO_BASE + 0x0024)
#define GPIO_ENABLE_W1TC_REG        (DR_REG_GPIO_BASE + 0x0028)
#define GPIO_ENABLE1_REG            (DR_REG_GPIO_BASE + 0x002C)
#define GPIO_ENABLE1_W1TS_REG       (DR_REG_GPIO_BASE + 0x0030)
#define GPIO_ENABLE1_W1TC_REG       (DR_REG_GPIO_BASE + 0x0034)

/* GPIO Input Registers */
#define GPIO_IN_REG                 (DR_REG_GPIO_BASE + 0x003C)
#define GPIO_IN1_REG                (DR_REG_GPIO_BASE + 0x0040)

/* GPIO Interrupt Status */
#define GPIO_STATUS_REG             (DR_REG_GPIO_BASE + 0x0044)
#define GPIO_STATUS_W1TS_REG        (DR_REG_GPIO_BASE + 0x0048)
#define GPIO_STATUS_W1TC_REG        (DR_REG_GPIO_BASE + 0x004C)
#define GPIO_STATUS1_REG            (DR_REG_GPIO_BASE + 0x0050)
#define GPIO_STATUS1_W1TS_REG       (DR_REG_GPIO_BASE + 0x0054)
#define GPIO_STATUS1_W1TC_REG       (DR_REG_GPIO_BASE + 0x0058)

/* GPIO Pin Configuration (per-pin) */
#define GPIO_PIN_REG(n)             (DR_REG_GPIO_BASE + 0x0074 + ((n) * 4))

/* GPIO_PIN_REG bit fields */
#define GPIO_PIN_PAD_DRIVER         (1 << 2)    /* 0=push-pull, 1=open-drain */
#define GPIO_PIN_INT_TYPE_S         7
#define GPIO_PIN_INT_TYPE_M         (0x7 << GPIO_PIN_INT_TYPE_S)
#define GPIO_PIN_INT_ENA_S          13
#define GPIO_PIN_INT_ENA_M          (0x1F << GPIO_PIN_INT_ENA_S)

/* Interrupt types */
#define GPIO_INT_DISABLE            0
#define GPIO_INT_POSEDGE            1
#define GPIO_INT_NEGEDGE            2
#define GPIO_INT_ANYEDGE            3
#define GPIO_INT_LOW_LEVEL          4
#define GPIO_INT_HIGH_LEVEL         5

/* GPIO Function Select (matrix output) */
#define GPIO_FUNC_OUT_SEL_CFG_REG(n) (DR_REG_GPIO_BASE + 0x0554 + ((n) * 4))
#define GPIO_FUNC_OUT_SEL_S         0
#define GPIO_FUNC_OUT_SEL_M         (0x1FF << GPIO_FUNC_OUT_SEL_S)
#define GPIO_FUNC_OUT_INV_SEL       (1 << 9)
#define GPIO_FUNC_OEN_SEL           (1 << 10)
#define GPIO_FUNC_OEN_INV_SEL       (1 << 11)

/* GPIO Matrix Input Select */
#define GPIO_FUNC_IN_SEL_CFG_REG(sig) (DR_REG_GPIO_BASE + 0x0154 + ((sig) * 4))

/* Special function values */
#define GPIO_FUNC_SIG_GPIO          256         /* Connect to GPIO directly */

/* IO MUX Register per GPIO */
#define IO_MUX_GPIO_REG(n)          (DR_REG_IO_MUX_BASE + 0x0004 + ((n) * 4))

/* IO_MUX bit fields */
#define IO_MUX_MCU_OE               (1 << 0)
#define IO_MUX_SLP_SEL              (1 << 1)
#define IO_MUX_MCU_WPD              (1 << 2)
#define IO_MUX_MCU_WPU              (1 << 3)
#define IO_MUX_MCU_IE               (1 << 4)
#define IO_MUX_FUN_WPD              (1 << 7)
#define IO_MUX_FUN_WPU              (1 << 8)
#define IO_MUX_FUN_IE               (1 << 9)
#define IO_MUX_FUN_DRV_S            10
#define IO_MUX_FUN_DRV_M            (0x3 << IO_MUX_FUN_DRV_S)
#define IO_MUX_MCU_SEL_S            12
#define IO_MUX_MCU_SEL_M            (0x7 << IO_MUX_MCU_SEL_S)
#define IO_MUX_FILTER_EN            (1 << 15)

/* Function select values */
#define IO_MUX_FUNC_GPIO            1

/* GPIO count */
#define GPIO_NUM_MAX                49

#endif /* REG_GPIO_REG_H */
