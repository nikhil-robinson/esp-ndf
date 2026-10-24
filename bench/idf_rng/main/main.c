/* IDF sibling of examples/rng_test: esp_random() per word, esp_fill_random() 4 KB / 64 KB (RNG entropy source on, as
 * bootloader_random_enable), same board and clock. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "esp_cpu.h"
#include "bootloader_random.h"
static uint8_t big[65536];
void app_main(void)
{
    vTaskDelay(300);
    bootloader_random_enable();
    uint32_t t = esp_cpu_get_cycle_count(); for (int i = 0; i < 1024; i++) ((uint32_t *)big)[i] = esp_random();
    uint32_t w = (esp_cpu_get_cycle_count() - t) / 1024;
    uint32_t u = ~0u; for (int i = 0; i < 8; i++) { uint32_t c = esp_cpu_get_cycle_count(); (void)esp_random(); c = esp_cpu_get_cycle_count() - c; if (c < u) u = c; }
    t = esp_cpu_get_cycle_count(); esp_fill_random(big, 4096); uint32_t f4 = esp_cpu_get_cycle_count() - t;
    t = esp_cpu_get_cycle_count(); esp_fill_random(big, sizeof big); uint32_t f64 = esp_cpu_get_cycle_count() - t;
    printf("idf speed: esp_random %lu cycles/word in a loop (%lu us), single call %lu cycles, esp_fill_random 4 KB %lu us, 64 KB %lu us\n",
           (unsigned long)w, (unsigned long)(w / 240), (unsigned long)u, (unsigned long)(f4 / 240), (unsigned long)(f64 / 240));
    printf("idf_rng done\n");
    for (;;) vTaskDelay(1000);
}
