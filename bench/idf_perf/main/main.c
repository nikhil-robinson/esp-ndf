/* IDF sibling of components/arch/test_apps/debug_test: perfmon counter read cost and counts for the same 1000 x 10 nop loop. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xtensa_perfmon_access.h"
#include "xtensa/xt_perf_consts.h"
#include "esp_cpu.h"
void app_main(void)
{
    vTaskDelay(300);
    xtensa_perfmon_init(0, XTPERF_CNT_CYCLES, 1, 0, -1);
    xtensa_perfmon_init(1, XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL, 0, -1);
    xtensa_perfmon_start();
    uint32_t c0 = esp_cpu_get_cycle_count(), p0 = xtensa_perfmon_value(0), i0 = xtensa_perfmon_value(1);
    __asm__ volatile("movi a8, 1000\n loop a8, 1f\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n 1:" ::: "a8");
    uint32_t c1 = esp_cpu_get_cycle_count(), p1 = xtensa_perfmon_value(0), i1 = xtensa_perfmon_value(1);
    uint32_t best = ~0u;
    for (int k = 0; k < 100; k++) { uint32_t r0 = esp_cpu_get_cycle_count(); (void)xtensa_perfmon_value(0); uint32_t d = esp_cpu_get_cycle_count() - r0; if (d < best) best = d; }
    printf("idf perf: cycles %lu (CCOUNT %lu), instructions %lu; read %lu cycles (incl. ccount)\n", (unsigned long)(p1 - p0),
           (unsigned long)(c1 - c0), (unsigned long)(i1 - i0), (unsigned long)best);
    printf("idf_perf done\n");
    for (;;) vTaskDelay(1000);
}
