/*
 * ESP32-S3 System Control
 */

#include "esp32s3.h"

/* System register offsets */
#define SYSTEM_PERIP_CLK_EN0_REG    (DR_REG_SYSTEM_BASE + 0x0010)
#define SYSTEM_PERIP_CLK_EN1_REG    (DR_REG_SYSTEM_BASE + 0x0014)
#define SYSTEM_PERIP_RST_EN0_REG    (DR_REG_SYSTEM_BASE + 0x0018)
#define SYSTEM_PERIP_RST_EN1_REG    (DR_REG_SYSTEM_BASE + 0x001C)

/* Clock enable bits */
#define SYSTEM_UART_CLK_EN          (1 << 2)
#define SYSTEM_SPI2_CLK_EN          (1 << 6)
#define SYSTEM_I2C_EXT0_CLK_EN      (1 << 7)
#define SYSTEM_LEDC_CLK_EN          (1 << 11)
#define SYSTEM_TIMERGROUP_CLK_EN    (1 << 13)
#define SYSTEM_TIMERGROUP1_CLK_EN   (1 << 14)

void system_enable_peripheral_clock(uint32_t periph_mask)
{
    REG_SET_BIT(SYSTEM_PERIP_CLK_EN0_REG, periph_mask);
}

void system_disable_peripheral_clock(uint32_t periph_mask)
{
    REG_CLR_BIT(SYSTEM_PERIP_CLK_EN0_REG, periph_mask);
}

void system_reset_peripheral(uint32_t periph_mask)
{
    REG_SET_BIT(SYSTEM_PERIP_RST_EN0_REG, periph_mask);
    nop_delay(10);
    REG_CLR_BIT(SYSTEM_PERIP_RST_EN0_REG, periph_mask);
}
