/* IDF sibling of examples/fastmath_test: newlib float math on the same arguments, cycles per call. */
#include <stdio.h>
#include <math.h>
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fastmath_bench.h"
#define TIME(label, expr) do { uint32_t t = esp_cpu_get_cycle_count(); for (int i = 0; i < FM_N; i++) fm_o[i] = (expr); t = esp_cpu_get_cycle_count() - t; \
    printf("idf %-14s %5lu.%lu cycles/call\n", label, (unsigned long)(t / FM_N), (unsigned long)(t % FM_N * 10 / FM_N)); } while (0)
void app_main(void)
{
    vTaskDelay(100);
    fm_args();
    for (int k = 0; k < 2; k++) {
        TIME("sinf libm", sinf(fm_x[i])); TIME("cosf libm", cosf(fm_x[i])); TIME("atan2f libm", atan2f(fm_y[i], fm_x[i]));
        TIME("expf libm", expf(fm_x[i])); TIME("logf libm", logf(fm_x[i] + 10.5f)); TIME("sqrtf libm", sqrtf(fm_x[i] + 10.5f));
    }
    printf("idf_math done\n");
    for (;;) vTaskDelay(1000);
}
