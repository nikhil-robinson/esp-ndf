#include "test.h"
#include "gpio.h"
#include "sys.h"

#define P 4     /* loopback through the pad itself: output + input enabled */

static volatile uint32_t isr_t, isr_n;
RAM_FN static void pin_isr(void *arg) { isr_t = ccount(); isr_n++; (void)arg; }

void test_gpio(void)
{
    CHECK(gpio_config(19, GPIO_OUTPUT) < 0);           /* console pin refused */
    CHECK(gpio_config(P, GPIO_OUTPUT | GPIO_INPUT) == 0);
    gpio_set(P); CHECK(gpio_read(P) == 1);
    gpio_clr(P); CHECK(gpio_read(P) == 0);
    gpio_toggle(P); CHECK(gpio_read(P) == 1);
    gpio_clr(P);
    gpio_config(40, GPIO_OUTPUT | GPIO_INPUT);         /* high bank */
    gpio_set(40); CHECK(gpio_read(40) == 1); gpio_clr(40); CHECK(gpio_read(40) == 0);

    METRIC("gpio_set+gpio_clr", "%lu cycles", CYCLES(gpio_set(P); gpio_clr(P)));

    /* edge interrupt: latency from the write to the handler's first line */
    CHECK(gpio_irq_attach(P, GPIO_IRQ_RISING, 1, pin_isr, 0) == 0);
    irq_enable_all();
    uint32_t best = ~0u, worst = 0;
    for (int i = 0; i < 1000; i++) {
        gpio_clr(P);
        uint32_t n = isr_n, t0 = ccount();
        gpio_set(P);
        while (isr_n == n) {}
        uint32_t d = isr_t - t0;
        if (d < best) best = d;
        if (d > worst) worst = d;
    }
    CHECK(isr_n == 1000);
    gpio_clr(P);
    /* falling edges must not fire; edges closer than the ISR latency merge
     * in hardware, so space them out */
    uint32_t n = isr_n;
    for (int i = 0; i < 3; i++) { gpio_set(P); delay_us(5); gpio_clr(P); delay_us(5); }
    CHECK(isr_n == n + 3);
    gpio_irq_detach(P);
    METRIC("pin write -> ISR (rising, L1)", "min %lu max %lu cycles", best, worst);

    /* dedicated GPIO channel 0 on P */
    CHECK(dgpio_route(0, P, true) == 0);
    dgpio_write(1, 1); CHECK(gpio_read(P) == 1);
    dgpio_write(1, 0); CHECK(gpio_read(P) == 0);
    METRIC("dgpio_write x2", "%lu cycles", CYCLES(dgpio_write(1, 1); dgpio_write(1, 0)));
    gpio_config(P, GPIO_OUTPUT | GPIO_INPUT);
}

NDF_UNIT(020, gpio);
