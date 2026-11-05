/* System pads are never handed out. */
#include "unit.h"
#include "gpio.h"
TEST(gpio_system_pads_refused)                          /* flash / octal PSRAM pads cannot be taken, even forced */
{
#if SOC_CPU_RISCV                                       /* P4: flash and PSRAM pads are dedicated (not GPIOs); the USB pads 24 / 25 need GPIO_FORCE */
    UASSERT(gpio_config(55, GPIO_OUTPUT) < 0);
    UASSERT(gpio_config(24, GPIO_OUTPUT) < 0);
    UASSERT(gpio_config(25, GPIO_OUTPUT) < 0);
#else
    UASSERT(gpio_config(27, GPIO_OUTPUT) < 0);
    UASSERT(gpio_config(30, GPIO_OUTPUT | GPIO_FORCE) < 0);
    UASSERT(gpio_config(19, GPIO_OUTPUT) < 0);           /* USB-JTAG pads: only with GPIO_FORCE */
#endif
    UASSERT(gpio_config(4, GPIO_OUTPUT) == 0);
}

