/* ESP-IDF v6.1 twin of components/arch/test_apps/irq_latency (P4 branch): a FROM_CPU0 software interrupt through the
 * interrupt matrix, raised and taken on the same core, allocated with esp_intr_alloc at levels 1 / 3 and an IRAM C
 * handler. Latency = cycle count at handler entry - cycle count just before the trigger store. */
#include <stdio.h>
#include "esp_intr_alloc.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_clk_tree.h"
#include "soc/interrupts.h"
#include "soc/hp_system_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define N 20000
typedef struct { uint32_t min, max, n; uint64_t sum; volatile uint32_t t0, done; } lat_t;
static lat_t lat[2];

static void IRAM_ATTR sw_isr(void *arg)
{
    uint32_t now = esp_cpu_get_cycle_count();
    lat_t *l = arg;
    REG_WRITE(HP_SYSTEM_CPU_INT_FROM_CPU_2_REG, 0);
    uint32_t d = now - l->t0;
    if (d < l->min) l->min = d;
    if (d > l->max) l->max = d;
    l->sum += d; l->n++; l->done = 1;
}

void app_main(void)
{
    static const int flags[2] = { ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM, ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM };
    uint32_t hz;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &hz);
    uint32_t mhz = hz / 1000000;
    vTaskDelay(10);
    printf("\nESP-IDF interrupt latency, FROM_CPU2 (%d samples per level, cycles @ %lu MHz)\n", N, mhz);
    for (int k = 0; k < 2; k++) {
        lat_t *l = &lat[k];
        l->min = ~0u;
        intr_handle_t h;
        ESP_ERROR_CHECK(esp_intr_alloc(ETS_FROM_CPU_INTR2_SOURCE, flags[k], sw_isr, l, &h));
        for (int i = 0; i < N; i++) {
            l->done = 0;
            l->t0 = esp_cpu_get_cycle_count();
            REG_WRITE(HP_SYSTEM_CPU_INT_FROM_CPU_2_REG, 1);
            while (!l->done) {}
        }
        esp_intr_free(h);
        printf("level %d: min %3lu  avg %3lu  max %3lu cycles  = %lu / %lu ns\n", k ? 3 : 1,
               l->min, (uint32_t)(l->sum / l->n), l->max, l->min * 1000 / mhz, l->max * 1000 / mhz);
    }
}
