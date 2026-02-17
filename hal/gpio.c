/*
 * GPIO Hardware Abstraction Layer Implementation
 */

#include "gpio.h"
#include "../reg/gpio_reg.h"

void gpio_init(void)
{
    /* GPIO subsystem is always enabled on ESP32-S3 */
}

void gpio_config_pin(uint32_t gpio_num, const gpio_config_t *config)
{
    if (gpio_num >= GPIO_NUM_MAX) return;
    
    /* Configure IO MUX for GPIO function */
    uint32_t iomux_reg = IO_MUX_GPIO_REG(gpio_num);
    uint32_t val = REG_READ(iomux_reg);
    
    /* Clear function select, set to GPIO */
    val &= ~IO_MUX_MCU_SEL_M;
    val |= (IO_MUX_FUNC_GPIO << IO_MUX_MCU_SEL_S);
    
    /* Configure pull-up/pull-down */
    val &= ~(IO_MUX_FUN_WPU | IO_MUX_FUN_WPD);
    if (config->pull == GPIO_PULL_UP) {
        val |= IO_MUX_FUN_WPU;
    } else if (config->pull == GPIO_PULL_DOWN) {
        val |= IO_MUX_FUN_WPD;
    }
    
    /* Configure drive strength */
    val &= ~IO_MUX_FUN_DRV_M;
    val |= ((config->drive & 0x3) << IO_MUX_FUN_DRV_S);
    
    /* Enable input if needed */
    if (config->dir == GPIO_DIR_INPUT || config->dir == GPIO_DIR_INPUT_OUTPUT) {
        val |= IO_MUX_FUN_IE;
    } else {
        val &= ~IO_MUX_FUN_IE;
    }
    
    REG_WRITE(iomux_reg, val);
    
    /* Configure GPIO matrix for simple GPIO output */
    REG_WRITE(GPIO_FUNC_OUT_SEL_CFG_REG(gpio_num), GPIO_FUNC_SIG_GPIO);
    
    /* Set direction */
    gpio_set_direction(gpio_num, config->dir);
}

void gpio_set_direction(uint32_t gpio_num, gpio_dir_t dir)
{
    if (gpio_num >= GPIO_NUM_MAX) return;
    
    if (dir == GPIO_DIR_OUTPUT || dir == GPIO_DIR_INPUT_OUTPUT) {
        if (gpio_num < 32) {
            REG_WRITE(GPIO_ENABLE_W1TS_REG, (1 << gpio_num));
        } else {
            REG_WRITE(GPIO_ENABLE1_W1TS_REG, (1 << (gpio_num - 32)));
        }
    } else {
        if (gpio_num < 32) {
            REG_WRITE(GPIO_ENABLE_W1TC_REG, (1 << gpio_num));
        } else {
            REG_WRITE(GPIO_ENABLE1_W1TC_REG, (1 << (gpio_num - 32)));
        }
    }
}

void gpio_set_level(uint32_t gpio_num, uint32_t level)
{
    if (level) {
        gpio_set_high(gpio_num);
    } else {
        gpio_set_low(gpio_num);
    }
}

uint32_t gpio_get_level(uint32_t gpio_num)
{
    if (gpio_num >= GPIO_NUM_MAX) return 0;
    
    if (gpio_num < 32) {
        return (REG_READ(GPIO_IN_REG) >> gpio_num) & 1;
    } else {
        return (REG_READ(GPIO_IN1_REG) >> (gpio_num - 32)) & 1;
    }
}

void gpio_set_high(uint32_t gpio_num)
{
    if (gpio_num >= GPIO_NUM_MAX) return;
    
    if (gpio_num < 32) {
        REG_WRITE(GPIO_OUT_W1TS_REG, (1 << gpio_num));
    } else {
        REG_WRITE(GPIO_OUT1_W1TS_REG, (1 << (gpio_num - 32)));
    }
}

void gpio_set_low(uint32_t gpio_num)
{
    if (gpio_num >= GPIO_NUM_MAX) return;
    
    if (gpio_num < 32) {
        REG_WRITE(GPIO_OUT_W1TC_REG, (1 << gpio_num));
    } else {
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1 << (gpio_num - 32)));
    }
}

void gpio_toggle(uint32_t gpio_num)
{
    if (gpio_num >= GPIO_NUM_MAX) return;
    
    uint32_t current;
    if (gpio_num < 32) {
        current = (REG_READ(GPIO_OUT_REG) >> gpio_num) & 1;
    } else {
        current = (REG_READ(GPIO_OUT1_REG) >> (gpio_num - 32)) & 1;
    }
    
    if (current) {
        gpio_set_low(gpio_num);
    } else {
        gpio_set_high(gpio_num);
    }
}

void gpio_set_pull(uint32_t gpio_num, gpio_pull_t pull)
{
    if (gpio_num >= GPIO_NUM_MAX) return;
    
    uint32_t reg = IO_MUX_GPIO_REG(gpio_num);
    uint32_t val = REG_READ(reg);
    
    val &= ~(IO_MUX_FUN_WPU | IO_MUX_FUN_WPD);
    if (pull == GPIO_PULL_UP) {
        val |= IO_MUX_FUN_WPU;
    } else if (pull == GPIO_PULL_DOWN) {
        val |= IO_MUX_FUN_WPD;
    }
    
    REG_WRITE(reg, val);
}

void gpio_set_drive(uint32_t gpio_num, gpio_drive_t drive)
{
    if (gpio_num >= GPIO_NUM_MAX) return;
    
    uint32_t reg = IO_MUX_GPIO_REG(gpio_num);
    uint32_t val = REG_READ(reg);
    
    val &= ~IO_MUX_FUN_DRV_M;
    val |= ((drive & 0x3) << IO_MUX_FUN_DRV_S);
    
    REG_WRITE(reg, val);
}
