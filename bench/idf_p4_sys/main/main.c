/* IDF sibling of components/lib/test_apps/sys_p4: gpio_set_level, portENTER/EXIT_CRITICAL, esp_timer_get_time, xRingbuffer send + receive of a 64-byte item (cycles per operation). */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_cpu.h"
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
void app_main(void)
{
    vTaskDelay(300);
    gpio_set_direction(20, GPIO_MODE_OUTPUT);
    RingbufHandle_t rb = xRingbufferCreate(1024, RINGBUF_TYPE_NOSPLIT);
    uint8_t item[64] = { 1 }; volatile uint32_t sink = 0; uint32_t best, t;
#define BEST(name, stmt) do { best = ~0u; for (int r_ = 0; r_ < 8; r_++) { t = esp_cpu_get_cycle_count(); for (int i = 0; i < 100; i++) { stmt; } t = esp_cpu_get_cycle_count() - t; if (t < best) best = t; } \
        printf("idf  %-28s %5lu cycles\n", name, (unsigned long)best / 100); } while (0)
    BEST("gpio_write", gpio_set_level(20, i & 1));
    BEST("irq_lock + irq_unlock", { portENTER_CRITICAL(&mux); portEXIT_CRITICAL(&mux); });
    BEST("time_us", sink += (uint32_t)esp_timer_get_time());
    BEST("ringbuf put+get 64 B", { xRingbufferSend(rb, item, sizeof item, 0); size_t l; void *p = xRingbufferReceive(rb, &l, 0); vRingbufferReturnItem(rb, p); });
    printf("idf_p4_sys done\n");
    for (;;) vTaskDelay(1000);
}
