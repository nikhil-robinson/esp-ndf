/* IDF sibling of examples/gdma_ext: esp_async_memcpy (GDMA M2M) internal <-> PSRAM and PSRAM -> PSRAM, same sizes. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_async_memcpy.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
static SemaphoreHandle_t sem;
static bool IRAM_ATTR done(async_memcpy_handle_t h, async_memcpy_event_t *e, void *a) { BaseType_t w = 0; xSemaphoreGiveFromISR(sem, &w); return w; }
static uint32_t cp(async_memcpy_handle_t h, void *d, void *s, size_t n)
{
    uint32_t t = esp_cpu_get_cycle_count();
    for (size_t o = 0; o < n; o += 65536) { size_t k = n - o > 65536 ? 65536 : n - o; esp_async_memcpy(h, (uint8_t *)d + o, (uint8_t *)s + o, k, done, 0); xSemaphoreTake(sem, portMAX_DELAY); }
    return esp_cpu_get_cycle_count() - t;
}
void app_main(void)
{
    vTaskDelay(300);
    sem = xSemaphoreCreateBinary();
    async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG(); cfg.backlog = 8; cfg.psram_trans_align = 64; cfg.sram_trans_align = 4;
    async_memcpy_handle_t h; int rc = esp_async_memcpy_install(&cfg, &h);
    const size_t N = 1 << 20;
    uint8_t *ib = heap_caps_aligned_alloc(64, 65536, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint8_t *p1 = heap_caps_aligned_alloc(64, N, MALLOC_CAP_SPIRAM), *p2 = heap_caps_aligned_alloc(64, N, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 65536; i++) ib[i] = i * 13;
    uint32_t tw = 0; for (size_t o = 0; o < N; o += 65536) tw += cp(h, p1 + o, ib, 65536);
    uint32_t tr = cp(h, ib, p1 + 0x40000, 65536);
    uint32_t tpp = cp(h, p2, p1, N);
    int ok = rc == 0 && ib[100] == (uint8_t)(100 * 13) && p2[N - 1] == (uint8_t)((65535) * 13);
    printf("idf PSRAM DMA: ok %d, write 1 MB %lu us (%lu MB/s), read 64 KB %lu us (%lu MB/s), PSRAM->PSRAM 1 MB %lu us (%lu MB/s)\n", ok,
           (unsigned long)(tw / 240), (unsigned long)((uint64_t)N * 240 / tw), (unsigned long)(tr / 240), (unsigned long)(65536ull * 240 / tr),
           (unsigned long)(tpp / 240), (unsigned long)((uint64_t)N * 240 / tpp));
    printf("idf_gdma done\n");
    for (;;) vTaskDelay(1000);
}
