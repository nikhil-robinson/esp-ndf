/* IDF sibling of components/soc/test_apps/mem_test: malloc+free cycles (best of 16) for 64 B, 4 KB (internal) and 100 KB (PSRAM via
 * SPIRAM_USE_MALLOC), heap_caps_malloc SPIRAM. */
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
static uint32_t cyc(size_t n, uint32_t caps)
{
    uint32_t best = ~0u;
    for (int i = 0; i < 16; i++) { uint32_t c = esp_cpu_get_cycle_count(); void *p = caps ? heap_caps_malloc(n, caps) : malloc(n); free(p); c = esp_cpu_get_cycle_count() - c; if (c < best) best = c; }
    return best;
}
void app_main(void)
{
    vTaskDelay(300);
    void *b = malloc(100000);
    printf("idf: PSRAM free %u, malloc(100000) at %p\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), b); free(b);
    printf("idf speed: malloc+free 64 B %lu cycles, 4 KB %lu, 100 KB (PSRAM) %lu, heap_caps SPIRAM 64 B %lu\n",
           (unsigned long)cyc(64, 0), (unsigned long)cyc(4096, 0), (unsigned long)cyc(100000, 0), (unsigned long)cyc(64, MALLOC_CAP_SPIRAM));
    printf("idf_mem done\n");
    for (;;) vTaskDelay(1000);
}
