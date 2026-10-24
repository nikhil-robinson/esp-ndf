/* IDF sibling of examples/xcore_call: esp_ipc_call_blocking round trip. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ipc.h"
#include "esp_cpu.h"
static volatile int r;
static void where(void *a) { r = esp_cpu_get_core_id() * 1000 + (int)a; }
static void run(const char *name, int isr)
{
    uint32_t mn = ~0u, sum = 0; int ok = 1;
    for (int i = 0; i < 10000; i++) {
        uint32_t t = esp_cpu_get_cycle_count();
        esp_ipc_call_blocking(1, where, (void *)i);
        t = esp_cpu_get_cycle_count() - t; ok &= r == 1000 + i; if (t < mn) mn = t; sum += t;
    }
    printf("idf %s: ok %d, round trip min %lu avg %lu cycles\n", name, ok, (unsigned long)mn, (unsigned long)(sum / 10000));
}
void app_main(void)
{
    vTaskDelay(300);
    run("esp_ipc_call_blocking", 0);
    printf("idf_xcall done\n");
    for (;;) vTaskDelay(1000);
}
