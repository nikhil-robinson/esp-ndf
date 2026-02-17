/*
 * GPIO Hardware Abstraction Layer
 */

#ifndef HAL_GPIO_H
#define HAL_GPIO_H

#include <stdint.h>
#include <stdbool.h>

/* GPIO direction */
typedef enum {
    GPIO_DIR_INPUT = 0,
    GPIO_DIR_OUTPUT = 1,
    GPIO_DIR_INPUT_OUTPUT = 2
} gpio_dir_t;

/* GPIO pull mode */
typedef enum {
    GPIO_PULL_NONE = 0,
    GPIO_PULL_UP = 1,
    GPIO_PULL_DOWN = 2
} gpio_pull_t;

/* GPIO drive strength */
typedef enum {
    GPIO_DRIVE_0 = 0,   /* ~5mA */
    GPIO_DRIVE_1 = 1,   /* ~10mA */
    GPIO_DRIVE_2 = 2,   /* ~20mA */
    GPIO_DRIVE_3 = 3    /* ~40mA */
} gpio_drive_t;

/* GPIO interrupt type */
typedef enum {
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_POSEDGE = 1,
    GPIO_INTR_NEGEDGE = 2,
    GPIO_INTR_ANYEDGE = 3,
    GPIO_INTR_LOW = 4,
    GPIO_INTR_HIGH = 5
} gpio_intr_t;

/* GPIO configuration */
typedef struct {
    gpio_dir_t dir;
    gpio_pull_t pull;
    gpio_drive_t drive;
    gpio_intr_t intr;
} gpio_config_t;

/* Initialize GPIO subsystem */
void gpio_init(void);

/* Configure a GPIO pin */
void gpio_config_pin(uint32_t gpio_num, const gpio_config_t *config);

/* Set GPIO as output */
void gpio_set_direction(uint32_t gpio_num, gpio_dir_t dir);

/* Set GPIO level */
void gpio_set_level(uint32_t gpio_num, uint32_t level);

/* Get GPIO level */
uint32_t gpio_get_level(uint32_t gpio_num);

/* Set GPIO high */
void gpio_set_high(uint32_t gpio_num);

/* Set GPIO low */
void gpio_set_low(uint32_t gpio_num);

/* Toggle GPIO */
void gpio_toggle(uint32_t gpio_num);

/* Set pull mode */
void gpio_set_pull(uint32_t gpio_num, gpio_pull_t pull);

/* Set drive strength */
void gpio_set_drive(uint32_t gpio_num, gpio_drive_t drive);

#endif /* HAL_GPIO_H */
