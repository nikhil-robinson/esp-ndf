/* =============================================================================
 * gpio.c  –  ESP32-S3 GPIO driver implementation
 * ============================================================================= */
#include "drivers/gpio/gpio.h"
#include "soc/soc.h"

/* ---- Internal ISR dispatch table ---------------------------------------- */
static gpio_isr_t  _gpio_isr[GPIO_NUM_MAX];
static void       *_gpio_isr_arg[GPIO_NUM_MAX];

/* ---- CPU interrupt number allocated for GPIO (level 1) ------------------ */
static int _gpio_cpu_intr = -1;

/* ============================================================================
 * Private helpers
 * ============================================================================ */

/** Validate GPIO number (returns -1 for the non-existent GPIO22-25 range) */
static int gpio_valid(uint32_t n)
{
    if (n > 48) return -1;
    if (n >= 22 && n <= 25) return -1;
    return 0;
}

/**
 * Configure the IO MUX for a pin in GPIO matrix mode.
 * Sets MCU_SEL = IO_MUX_FUNC_GPIO (2), enables input, and configures pull.
 */
static void iomux_set_gpio(uint32_t gpio_num,
                           bool     input_en,
                           gpio_pull_t pull,
                           uint8_t  drive)
{
    if (!io_mux_gpio_reg[gpio_num]) return;   /* unmapped pad */

    uint32_t val = 0;
    val |= (IO_MUX_FUNC_GPIO << IO_MUX_MCU_SEL_S);  /* connect to GPIO matrix */
    val |= ((drive & 0x3) << IO_MUX_FUN_DRV_S);     /* drive strength         */
    if (input_en)              val |= IO_MUX_FUN_IE_M;
    if (pull == GPIO_PULL_UP   || pull == GPIO_PULL_UPDOWN) val |= IO_MUX_FUN_WPU_M;
    if (pull == GPIO_PULL_DOWN || pull == GPIO_PULL_UPDOWN) val |= IO_MUX_FUN_WPD_M;

    REG_WRITE(io_mux_gpio_reg[gpio_num], val);
}

/**
 * Connect this GPIO to the GPIO matrix in output mode.
 * OUT_SEL = GPIO_OUT_SIG_SW_CTRL (256) means "software control via GPIO_OUT".
 * OEN_SEL = 1 means "override output-enable from GPIO_ENABLE register".
 */
static void gpio_matrix_output_enable(uint32_t gpio_num, bool open_drain)
{
    uint32_t val = (GPIO_OUT_SIG_SW_CTRL << GPIO_OUT_SEL_S) |
                   (1u << GPIO_OUT_OEN_SEL_S);  /* OEN from GPIO_ENABLE */
    REG_WRITE(GPIO_FUNC_OUT_SEL_CFG_REG(gpio_num), val);

    /* Set or clear open-drain bit in GPIO_PIN_REG */
    uint32_t pin_reg = REG_READ(GPIO_PIN_REG(gpio_num));
    if (open_drain) {
        pin_reg |=  GPIO_PIN_PAD_DRIVER_M;
    } else {
        pin_reg &= ~GPIO_PIN_PAD_DRIVER_M;
    }
    REG_WRITE(GPIO_PIN_REG(gpio_num), pin_reg);
}

/** Enable / disable output for a GPIO in the GPIO_ENABLE register */
static void gpio_output_set(uint32_t gpio_num, bool enable)
{
    if (gpio_num < 32) {
        if (enable) REG_WRITE(GPIO_ENABLE_W1TS_REG, 1u << gpio_num);
        else        REG_WRITE(GPIO_ENABLE_W1TC_REG, 1u << gpio_num);
    } else {
        if (enable) REG_WRITE(GPIO_ENABLE1_W1TS_REG, 1u << (gpio_num - 32));
        else        REG_WRITE(GPIO_ENABLE1_W1TC_REG, 1u << (gpio_num - 32));
    }
}

/* ============================================================================
 * Interrupt handler (called from intr.c level-1 dispatch)
 * ============================================================================ */
static void IRAM_ATTR gpio_intr_handler(void *arg)
{
    (void)arg;

    /* Read and clear GPIO interrupt status registers */
    uint32_t st0 = REG_READ(GPIO_STATUS_REG);
    uint32_t st1 = REG_READ(GPIO_STATUS1_REG);

    REG_WRITE(GPIO_STATUS_W1TC_REG,  st0);
    REG_WRITE(GPIO_STATUS1_W1TC_REG, st1);

    /* Dispatch to per-pin callbacks */
    uint32_t n;
    for (n = 0; n < 32 && st0; n++) {
        if ((st0 >> n) & 1u) {
            if (_gpio_isr[n]) _gpio_isr[n](n, _gpio_isr_arg[n]);
        }
    }
    for (n = 32; n < GPIO_NUM_MAX && st1; n++) {
        if ((st1 >> (n - 32)) & 1u) {
            if (_gpio_isr[n]) _gpio_isr[n](n, _gpio_isr_arg[n]);
        }
    }
}

/* ============================================================================
 * Public API implementation
 * ============================================================================ */

int gpio_set_direction(uint32_t gpio_num, gpio_mode_t mode)
{
    if (gpio_valid(gpio_num) < 0) return -1;

    bool    input_en   = false;
    bool    output_en  = false;
    bool    open_drain = false;
    gpio_pull_t pull   = GPIO_PULL_NONE;
    uint8_t drive      = IO_MUX_DRV_20MA;

    switch (mode) {
    case GPIO_MODE_DISABLE:
        break;
    case GPIO_MODE_INPUT:
        input_en = true;
        break;
    case GPIO_MODE_INPUT_PULLUP:
        input_en = true;
        pull     = GPIO_PULL_UP;
        break;
    case GPIO_MODE_INPUT_PULLDOWN:
        input_en = true;
        pull     = GPIO_PULL_DOWN;
        break;
    case GPIO_MODE_OUTPUT:
        output_en = true;
        break;
    case GPIO_MODE_OUTPUT_OD:
        output_en  = true;
        open_drain = true;
        break;
    case GPIO_MODE_INPUT_OUTPUT:
        input_en  = true;
        output_en = true;
        break;
    case GPIO_MODE_INPUT_OUTPUT_OD:
        input_en   = true;
        output_en  = true;
        open_drain = true;
        break;
    }

    /* Set IO MUX to GPIO matrix mode */
    iomux_set_gpio(gpio_num, input_en, pull, drive);

    /* Configure output path in GPIO matrix */
    if (output_en) {
        gpio_matrix_output_enable(gpio_num, open_drain);
        gpio_output_set(gpio_num, true);
    } else {
        gpio_output_set(gpio_num, false);
    }

    return 0;
}


int gpio_set_intr_type(uint32_t gpio_num, gpio_intr_type_t intr_type)
{
    if (gpio_valid(gpio_num) < 0) return -1;

    uint32_t pin_reg = REG_READ(GPIO_PIN_REG(gpio_num));
    pin_reg &= ~GPIO_PIN_INT_TYPE_M;
    pin_reg |= (intr_type << GPIO_PIN_INT_TYPE_S);
    REG_WRITE(GPIO_PIN_REG(gpio_num), pin_reg);

    return 0;
}


int gpio_register_isr(uint32_t gpio_num, gpio_isr_t isr, void *arg)
{
    if (gpio_valid(gpio_num) < 0) return -1;

    /* Allocate a CPU interrupt for GPIO (level 1) if not already done */
    if (_gpio_cpu_intr < 0) {
        intr_alloc(INTR_SRC_GPIO_INTR, INTR_LEVEL_1,
                   gpio_intr_handler, NULL, &_gpio_cpu_intr);
        intr_enable(_gpio_cpu_intr);
    }

    _gpio_isr[gpio_num]     = isr;
    _gpio_isr_arg[gpio_num] = arg;

    /* Enable this GPIO's CPU interrupt contribution (bit 13 = PRO CPU intr 0) */
    uint32_t pin_reg = REG_READ(GPIO_PIN_REG(gpio_num));
    pin_reg |= (1u << GPIO_PIN_INT_ENA_S);   /* bit 13: enable PRO CPU (core0) intr */
    REG_WRITE(GPIO_PIN_REG(gpio_num), pin_reg);

    return 0;
}


void gpio_intr_enable(uint32_t gpio_num)
{
    if (gpio_valid(gpio_num) < 0) return;

    uint32_t pin_reg = REG_READ(GPIO_PIN_REG(gpio_num));
    pin_reg |= (1u << GPIO_PIN_INT_ENA_S);
    REG_WRITE(GPIO_PIN_REG(gpio_num), pin_reg);
}


void gpio_intr_disable_pin(uint32_t gpio_num)
{
    if (gpio_valid(gpio_num) < 0) return;

    uint32_t pin_reg = REG_READ(GPIO_PIN_REG(gpio_num));
    pin_reg &= ~GPIO_PIN_INT_ENA_M;
    REG_WRITE(GPIO_PIN_REG(gpio_num), pin_reg);
}


int gpio_set_drive_strength(uint32_t gpio_num, uint8_t drv)
{
    if (gpio_valid(gpio_num) < 0) return -1;
    if (!io_mux_gpio_reg[gpio_num]) return -1;

    uint32_t mux = REG_READ(io_mux_gpio_reg[gpio_num]);
    mux &= ~(0x3u << IO_MUX_FUN_DRV_S);
    mux |= ((drv & 0x3) << IO_MUX_FUN_DRV_S);
    REG_WRITE(io_mux_gpio_reg[gpio_num], mux);

    return 0;
}
