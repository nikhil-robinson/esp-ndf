/* IDF sibling of components/soc/test_apps/mem_test: malloc+free cycles (best of 16) for 64 B, 4 KB (internal) and 100 KB (PSRAM via
 * SPIRAM_USE_MALLOC), heap_caps_malloc SPIRAM. */
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include <string.h>
static uint8_t A[8192 + 64] __attribute__((aligned(16))), B[8192 + 64] __attribute__((aligned(16)));
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
    for (int i = 0; i < (int)sizeof A; i++) A[i] = i * 131 + 7;
    static const int sz[] = { 16, 64, 256, 1024, 8192 };
    for (int k = 0; k < 5; k++) {
        int n = sz[k]; uint32_t t, ta = ~0u, tm = ~0u, ts = ~0u, tv = ~0u;
        for (int r = 0; r < 4; r++) {                  /* best of four: the first run is cold */
            t = esp_cpu_get_cycle_count(); memcpy(B, A, n); t = esp_cpu_get_cycle_count() - t; if (t < ta) ta = t;
            t = esp_cpu_get_cycle_count(); memcpy(B + 1, A + 2, n); t = esp_cpu_get_cycle_count() - t; if (t < tm) tm = t;
            t = esp_cpu_get_cycle_count(); memset(B, 0x33, n); t = esp_cpu_get_cycle_count() - t; if (t < ts) ts = t;
            t = esp_cpu_get_cycle_count(); memmove(B + 4, B, n); t = esp_cpu_get_cycle_count() - t; if (t < tv) tv = t;
        }
        printf("idf %5d B: memcpy aligned %5lu  mixed %5lu  memset %5lu  memmove %5lu\n", n, (unsigned long)ta, (unsigned long)tm, (unsigned long)ts, (unsigned long)tv);
    }
    printf("idf_mem done\n");
    for (;;) vTaskDelay(1000);
}
