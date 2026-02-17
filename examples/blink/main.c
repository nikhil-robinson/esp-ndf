/*
 * Blink Example - ESP32-S3 Bare Metal
 * Toggles GPIO (onboard LED) on and off
 * With debug output via USB Serial/JTAG
 */

#include "hal/gpio.h"
#include "hal/debug.h"
#include "arch/esp32s3/esp32s3.h"

/* LED GPIO - adjust for your board */
#define LED_GPIO    1

/* Simple delay using NOP loop */
static void delay_ms(uint32_t ms)
{
    /* Approximate delay at 40MHz default clock */
    volatile uint32_t count = ms * 8000;
    while (count--) {
        __asm__ volatile ("nop");
    }
}

void main(void)
{
    /* Initialize debug output via USB Serial/JTAG (CDC on /dev/ttyACM0) */
    debug_init(DEBUG_USB_SERIAL);
    
    /* Delay to let USB CDC enumerate and host terminal connect */
    delay_ms(500);
    
    /* Output test - single characters first */
    debug_putc('H');
    debug_putc('E');
    debug_putc('L');
    debug_putc('L');
    debug_putc('O');
    debug_putc('\r');
    debug_putc('\n');
    
    debug_println("=================================");
    debug_println("ESP32-S3 Bare Metal Blink Example");
    debug_println("=================================");
    debug_printf("LED GPIO: %d\n", LED_GPIO);
    
    /* Configure LED GPIO */
    gpio_config_t led_config = {
        .dir = GPIO_DIR_OUTPUT,
        .pull = GPIO_PULL_NONE,
        .drive = GPIO_DRIVE_2,
        .intr = GPIO_INTR_DISABLE
    };
    
    gpio_config_pin(LED_GPIO, &led_config);
    debug_println("GPIO configured as output");
    
    uint32_t count = 0;
    
    /* Blink loop */
    while (1) {
        gpio_set_high(LED_GPIO);
        debug_printf("[%u] LED ON\n", count);
        delay_ms(500);
        
        gpio_set_low(LED_GPIO);
        debug_printf("[%u] LED OFF\n", count);
        delay_ms(500);
        
        count++;
    }
}
