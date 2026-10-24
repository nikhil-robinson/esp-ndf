#include "test.h"
#include "ledc.h"
#include "gpio.h"

#define P 4

/* Measure one PWM period by polling the pad (resolution ~ one GPIO read). */
RAM_FN static void measure(uint32_t *period, uint32_t *high)   /* RAM: no cache misses in the timing loop (XIP=all) */
{
    uint32_t t = ccount();
    while (gpio_read(P) && ccount() - t < 24000000) {}
    while (!gpio_read(P) && ccount() - t < 24000000) {}
    uint32_t r1 = ccount();
    while (gpio_read(P)) {}
    uint32_t f = ccount();
    while (!gpio_read(P)) {}
    uint32_t r2 = ccount();
    *period = r2 - r1; *high = f - r1;
}

void test_ledc(void)
{
    uint64_t mhz = ledc_timer(0, 20000, 10);           /* 20 kHz, 10-bit */
    METRIC("asked 20 kHz/10 bit, got", "%lu.%03lu Hz", (uint32_t)(mhz / 1000), (uint32_t)(mhz % 1000));
    CHECK(mhz == 20000000);
    CHECK(ledc_channel(0, 0, P, 256) == 0);            /* 25 % */
    uint32_t per, hi;
    measure(&per, &hi);
    CHECK(per >= 11970 && per <= 12030);               /* 50 us = 12000 cycles, +-poll step */
    CHECK(hi >= 2970 && hi <= 3030);                    /* 25 % */
    METRIC("period / high (cycles)", "%lu / %lu", per, hi);
    ledc_duty(0, 768);
    for (volatile int i = 0; i < 20000; i++) {}
    measure(&per, &hi);
    METRIC("after ledc_duty(768): period/high", "%lu / %lu", per, hi);
    CHECK(hi >= 8970 && hi <= 9030);                    /* 75 % */
    METRIC("ledc_duty()", "%lu cycles", CYCLES(ledc_duty(0, 512)));
    uint64_t odd = ledc_timer(1, 33333, 8);            /* not exact: report the truth */
    METRIC("asked 33333 Hz/8 bit, got", "%lu.%03lu Hz", (uint32_t)(odd / 1000), (uint32_t)(odd % 1000));
    CHECK(odd > 33300000 && odd < 33370000);
    ledc_stop(0, 0);
    gpio_config(P, GPIO_OUTPUT | GPIO_INPUT);
}
