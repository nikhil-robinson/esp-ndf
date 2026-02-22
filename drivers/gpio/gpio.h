/* =============================================================================
 * gpio.h  –  ESP32-S3 GPIO driver API
 *
 * Supports 45 digital GPIOs (0-21, 26-48).  GPIOs 22-25 do not exist.
 *
 * Usage example:
 *   gpio_set_direction(2, GPIO_MODE_OUTPUT);
 *   gpio_set_level(2, 1);          // drive high
 *
 *   gpio_set_direction(4, GPIO_MODE_INPUT_PULLUP);
 *   int level = gpio_get_level(4); // read pin
 * ============================================================================= */
#ifndef NDF_GPIO_H
#define NDF_GPIO_H

#include "types.h"
#include "compiler.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "intr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- GPIO limits --------------------------------------------------------- */
#define GPIO_NUM_MAX   49       /* GPIO0 – GPIO48 (GPIO22-25 missing)        */

/* ---- GPIO modes ---------------------------------------------------------- */
typedef enum {
    GPIO_MODE_DISABLE          = 0,    /* Pin tri-stated (input disabled)    */
    GPIO_MODE_INPUT            = 1,    /* Input only                         */
    GPIO_MODE_OUTPUT           = 2,    /* Output, push-pull                  */
    GPIO_MODE_OUTPUT_OD        = 3,    /* Output, open-drain                 */
    GPIO_MODE_INPUT_OUTPUT     = 4,    /* Input + output (push-pull)         */
    GPIO_MODE_INPUT_OUTPUT_OD  = 5,    /* Input + output (open-drain)        */
    GPIO_MODE_INPUT_PULLUP     = 6,    /* Input with internal pull-up        */
    GPIO_MODE_INPUT_PULLDOWN   = 7,    /* Input with internal pull-down      */
} gpio_mode_t;

/* ---- GPIO pull resistor configuration ----------------------------------- */
typedef enum {
    GPIO_PULL_NONE   = 0,
    GPIO_PULL_UP     = 1,
    GPIO_PULL_DOWN   = 2,
    GPIO_PULL_UPDOWN = 3,   /* Keeper mode (both enabled) */
} gpio_pull_t;

/* ---- GPIO interrupt trigger type ---------------------------------------- */
typedef enum {
    GPIO_INTR_TYPE_DISABLE    = GPIO_INTR_DISABLE,
    GPIO_INTR_TYPE_POSEDGE    = GPIO_INTR_POSEDGE,
    GPIO_INTR_TYPE_NEGEDGE    = GPIO_INTR_NEGEDGE,
    GPIO_INTR_TYPE_ANYEDGE    = GPIO_INTR_ANYEDGE,
    GPIO_INTR_TYPE_LOW_LEVEL  = GPIO_INTR_LOW_LEVEL,
    GPIO_INTR_TYPE_HIGH_LEVEL = GPIO_INTR_HIGH_LEVEL,
} gpio_intr_type_t;

/* ---- GPIO ISR callback --------------------------------------------------- */
typedef void (*gpio_isr_t)(uint32_t gpio_num, void *arg);

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * @brief  Set the IO mode for a GPIO pin.
 *
 * Configures the IO MUX to connect the GPIO to the software-controlled GPIO
 * matrix (MCU_SEL = FUNC_GPIO), then sets direction, pull resistors, and
 * drive strength in the GPIO controller.
 *
 * @param gpio_num  GPIO number (0-21, 26-48)
 * @param mode      GPIO mode (direction + pull)
 * @return 0 on success, -1 on invalid GPIO number
 */
int gpio_set_direction(uint32_t gpio_num, gpio_mode_t mode);

/**
 * @brief  Set the output level of a GPIO pin.
 * @param gpio_num  GPIO number
 * @param level     0 = logic low, non-zero = logic high
 */
ALWAYS_INLINE void gpio_set_level(uint32_t gpio_num, int level)
{
    if (gpio_num < 32) {
        if (level) {
            REG_WRITE(GPIO_OUT_W1TS_REG, 1u << gpio_num);
        } else {
            REG_WRITE(GPIO_OUT_W1TC_REG, 1u << gpio_num);
        }
    } else if (gpio_num < GPIO_NUM_MAX) {
        if (level) {
            REG_WRITE(GPIO_OUT1_W1TS_REG, 1u << (gpio_num - 32));
        } else {
            REG_WRITE(GPIO_OUT1_W1TC_REG, 1u << (gpio_num - 32));
        }
    }
}

/**
 * @brief  Read the current logic level of a GPIO pin (input or output).
 * @param gpio_num  GPIO number
 * @return 0 or 1, -1 if invalid
 */
ALWAYS_INLINE int gpio_get_level(uint32_t gpio_num)
{
    if (gpio_num < 32) {
        return (REG_READ(GPIO_IN_REG)  >> gpio_num) & 1;
    } else if (gpio_num < GPIO_NUM_MAX) {
        return (REG_READ(GPIO_IN1_REG) >> (gpio_num - 32)) & 1;
    }
    return -1;
}

/**
 * @brief  Toggle the output level of a GPIO pin.
 */
ALWAYS_INLINE void gpio_toggle(uint32_t gpio_num)
{
    gpio_set_level(gpio_num, !gpio_get_level(gpio_num));
}

/**
 * @brief  Configure the interrupt trigger type for a GPIO.
 * @param gpio_num   GPIO number
 * @param intr_type  Trigger condition (edge/level)
 */
int gpio_set_intr_type(uint32_t gpio_num, gpio_intr_type_t intr_type);

/**
 * @brief  Register an ISR callback for a GPIO interrupt.
 *
 * The ISR is called from the GPIO interrupt handler (level-1 ISR) with the
 * triggering GPIO number and the user-provided argument.
 *
 * @param gpio_num  GPIO number (must have intr_type != DISABLE)
 * @param isr       Callback function
 * @param arg       Opaque argument passed to callback
 * @return 0 on success
 */
int gpio_register_isr(uint32_t gpio_num, gpio_isr_t isr, void *arg);

/**
 * @brief  Enable the GPIO interrupt for a pin.
 * gpio_set_intr_type() and gpio_register_isr() must be called first.
 */
void gpio_intr_enable(uint32_t gpio_num);

/**
 * @brief  Disable the GPIO interrupt for a pin.
 */
void gpio_intr_disable_pin(uint32_t gpio_num);

/**
 * @brief  Clear the GPIO interrupt status for a pin.
 * Call from within the ISR handler to acknowledge the interrupt.
 */
ALWAYS_INLINE void gpio_intr_clear(uint32_t gpio_num)
{
    if (gpio_num < 32) {
        REG_WRITE(GPIO_STATUS_W1TC_REG, 1u << gpio_num);
    } else if (gpio_num < GPIO_NUM_MAX) {
        REG_WRITE(GPIO_STATUS1_W1TC_REG, 1u << (gpio_num - 32));
    }
}

/**
 * @brief  Set the drive strength of a GPIO output.
 * @param gpio_num  GPIO number
 * @param drv       Drive strength (IO_MUX_DRV_5MA … IO_MUX_DRV_40MA)
 */
int gpio_set_drive_strength(uint32_t gpio_num, uint8_t drv);

#ifdef __cplusplus
}
#endif

#endif /* NDF_GPIO_H */
