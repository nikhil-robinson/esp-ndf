#include "t.h"
#include "driver/gpio.h"
#include "driver/dedic_gpio.h"
#define P 4
static volatile uint32_t isr_t, isr_n;
static void IRAM_ATTR pin_isr(void *arg) { isr_t = ccount(); isr_n++; }
void t_gpio(void)
{
    gpio_config_t c = { .pin_bit_mask = 1ULL << P, .mode = GPIO_MODE_INPUT_OUTPUT };
    gpio_config(&c);
    METRIC("gpio_set+gpio_clr", "%lu cycles", CYCLES(gpio_set_level(P, 1); gpio_set_level(P, 0)));
    gpio_set_intr_type(P, GPIO_INTR_POSEDGE);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(P, pin_isr, 0);
    uint32_t best = ~0u, worst = 0;
    for (int i = 0; i < 1000; i++) {
        gpio_set_level(P, 0);
        uint32_t n = isr_n, t0 = ccount();
        gpio_set_level(P, 1);
        while (isr_n == n) {}
        uint32_t d = isr_t - t0;
        if (d < best) best = d;
        if (d > worst) worst = d;
    }
    gpio_isr_handler_remove(P);
    METRIC("pin write -> ISR (rising, L1)", "min %lu max %lu cycles", best, worst);
    int pins[1] = { P };
    dedic_gpio_bundle_config_t bc = { .gpio_array = pins, .array_size = 1, .flags.out_en = 1 };
    dedic_gpio_bundle_handle_t b;
    dedic_gpio_new_bundle(&bc, &b);
    METRIC("dgpio_write x2", "%lu cycles", CYCLES(dedic_gpio_bundle_write(b, 1, 1); dedic_gpio_bundle_write(b, 1, 0)));
}
