/* IDF sibling of components/drivers/psram/test_apps/psram_p4: 1 MB written to and read back from the PSRAM in 16 KB blocks, cache lines written back /
 * dropped around each block (esp_cache_msync), 64 KB copies of the same data too. 200 MHz hex PSRAM, CPU 360 MHz. */
#include <stdio.h>
#include <string.h>
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static uint8_t sram_buf[16384] __attribute__((aligned(64)));
void app_main(void)
{
    vTaskDelay(300);
    uint8_t *dst = heap_caps_aligned_alloc(64, 1 << 20, MALLOC_CAP_SPIRAM);
    printf("idf psram %u, buf %p\n", (unsigned)esp_psram_get_size(), dst);
    for (size_t i = 0; i < sizeof sram_buf; i++) sram_buf[i] = i * 7;
    uint32_t w = ~0u, r = ~0u;
    for (int rep = 0; rep < 3; rep++) {
        uint32_t t = esp_cpu_get_cycle_count();
        for (int k = 0; k < 64; k++) { memcpy(dst + k * 16384, sram_buf, 16384); esp_cache_msync(dst + k * 16384, 16384, ESP_CACHE_MSYNC_FLAG_DIR_C2M); }
        t = esp_cpu_get_cycle_count() - t; if (t < w) w = t;
        t = esp_cpu_get_cycle_count();
        for (int k = 0; k < 64; k++) { esp_cache_msync(dst + k * 16384, 16384, ESP_CACHE_MSYNC_FLAG_DIR_M2C); memcpy(sram_buf, dst + k * 16384, 16384); }
        t = esp_cpu_get_cycle_count() - t; if (t < r) r = t;
    }
    printf("idf psram: write %lu MB/s, read %lu MB/s\n", (unsigned long)((uint64_t)(1 << 20) * 360 / w), (unsigned long)((uint64_t)(1 << 20) * 360 / r));
    printf("idf_p4_psram done\n");
    for (;;) vTaskDelay(1000);
}
