/* Same measurement as examples/irq_latency on ESP-IDF: CCOMPARE timer
 * interrupts allocated with esp_intr_alloc, IRAM C handler. */
#include <stdio.h>
#include "esp_intr_alloc.h"
#include "esp_attr.h"
#include "soc/interrupts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define N 20000
typedef struct { uint32_t min, max, n; uint64_t sum; volatile uint32_t target, done; } lat_t;
static lat_t lat[2];

static inline uint32_t ccount(void) { uint32_t c; __asm__ volatile("rsr.ccount %0" : "=r"(c)); return c; }

#define CC_HANDLER(i)                                                       \
static void IRAM_ATTR cc##i##_isr(void *arg)                               \
{                                                                          \
    uint32_t now = ccount();                                               \
    lat_t *l = arg;                                                        \
    uint32_t d = now - l->target;                                          \
    __asm__ volatile("wsr.ccompare" #i " %0" :: "r"(l->target - 1));      \
    if (d < l->min) l->min = d;                                            \
    if (d > l->max) l->max = d;                                            \
    l->sum += d; l->n++; l->done = 1;                                      \
}
CC_HANDLER(0)
CC_HANDLER(1)

void app_main(void)
{
    static const int src[2] = { ETS_INTERNAL_TIMER0_INTR_SOURCE, ETS_INTERNAL_TIMER1_INTR_SOURCE };
    static const int flags[2] = { ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM, ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM };
    intr_handler_t fn[2] = { cc0_isr, cc1_isr };
    vTaskDelay(10);
    printf("\nESP-IDF interrupt latency (%d samples per level, cycles @ 240 MHz)\n", N);
    for (int k = 0; k < 2; k++) {
        lat_t *l = &lat[k];
        l->min = ~0u;
        uint32_t far = ccount() - 1;
        if (k == 0) __asm__ volatile("wsr.ccompare0 %0" :: "r"(far));
        else        __asm__ volatile("wsr.ccompare1 %0" :: "r"(far));
        intr_handle_t h;
        ESP_ERROR_CHECK(esp_intr_alloc(src[k], flags[k], fn[k], l, &h));
        for (int i = 0; i < N; i++) {
            l->done = 0;
            l->target = ccount() + 2000;
            if (k == 0) __asm__ volatile("wsr.ccompare0 %0; rsync" :: "r"(l->target));
            else        __asm__ volatile("wsr.ccompare1 %0; rsync" :: "r"(l->target));
            while (!l->done) {}
        }
        esp_intr_free(h);
        printf("level %d: min %3lu  avg %3lu  max %3lu cycles  = %lu / %lu ns\n", k ? 3 : 1,
               l->min, (uint32_t)(l->sum / l->n), l->max, l->min * 25 / 6, l->max * 25 / 6);
    }
}
