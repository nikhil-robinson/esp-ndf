/* IDF sibling of components/soc/test_apps/pm_test: esp_pm DFS 240/80, CPU_FREQ_MAX lock acquire/release cost (cycles, best of 100). */
#include <stdio.h>
#include "esp_pm.h"
#include "esp_cpu.h"
#include "esp_timer.h"
static uint32_t mhz(void) { int64_t t0 = esp_timer_get_time(); uint32_t c0 = esp_cpu_get_cycle_count(); while (esp_timer_get_time() - t0 < 2000) {} return (esp_cpu_get_cycle_count() - c0) / 2000; }
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
void app_main(void)
{
    vTaskDelay(100);
    esp_pm_config_t c = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false };
    int r = esp_pm_configure(&c);
    esp_pm_lock_handle_t l; esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "t", &l);
    uint32_t bl = ~0u, bu = ~0u;
    for (int i = 0; i < 100; i++) {
        uint32_t c0 = esp_cpu_get_cycle_count(); esp_pm_lock_acquire(l); uint32_t c1 = esp_cpu_get_cycle_count(); esp_pm_lock_release(l); uint32_t c2 = esp_cpu_get_cycle_count();
        if (c1 - c0 < bl) bl = c1 - c0;
        if (c2 - c1 < bu) bu = c2 - c1;
    }
    uint32_t idle = mhz(); esp_pm_lock_acquire(l); uint32_t busy = mhz(); esp_pm_lock_release(l); uint32_t back = mhz();
    printf("idf DFS: configure %d, lock %lu cycles, unlock %lu cycles; CPU idle %lu MHz, locked %lu MHz, released %lu MHz\n", r,
           (unsigned long)bl, (unsigned long)bu, (unsigned long)idle, (unsigned long)busy, (unsigned long)back);
    printf("idf_pm done\n");
    for (;;) vTaskDelay(1000);
}
