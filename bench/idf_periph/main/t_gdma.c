#include "t.h"
#include "esp_async_memcpy.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#define LEN 32768
static volatile int done;
static bool IRAM_ATTR cb(async_memcpy_handle_t h, async_memcpy_event_t *e, void *a) { done = 1; return false; }
void t_gdma(void)
{
    uint32_t *src = heap_caps_malloc(LEN, MALLOC_CAP_DMA), *dst = heap_caps_malloc(LEN, MALLOC_CAP_DMA);
    for (int i = 0; i < LEN / 4; i++) { src[i] = i * 2654435761u; dst[i] = 0; }
    async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    async_memcpy_handle_t h; esp_async_memcpy_install(&cfg, &h);
    done = 0;
    uint32_t t = ccount();
    esp_async_memcpy(h, dst, src, LEN, cb, 0);
    while (!done) {}
    uint32_t c = ccount() - t;
    int bad = 0; for (int i = 0; i < LEN / 4; i++) bad += dst[i] != src[i];
    METRIC("DMA copy 32 KB", "%lu cycles (%lu MB/s), %d words wrong", c, (uint32_t)((uint64_t)LEN * 240 / c), bad);
}
