/* IDF sibling of examples/trace_test: esp_apptrace over JTAG, 64 records of 1020 B (u32 seq + pattern). */
#include <stdio.h>
#include "esp_app_trace.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint8_t rec[1020] __attribute__((aligned(4)));

void app_main(void)
{
    printf("trace_test: waiting for the host\n");
    while (!esp_apptrace_host_is_connected(ESP_APPTRACE_DEST_JTAG)) vTaskDelay(1);
    uint32_t best = ~0u, fails = 0, t0 = esp_cpu_get_cycle_count();
    for (uint32_t s = 0; s < 64; s++) {
        for (int i = 0; i < 1020; i++) rec[i] = (uint8_t)(s * 31 + i);
        *(uint32_t *)rec = s;
        uint32_t c = esp_cpu_get_cycle_count();
        if (esp_apptrace_write(ESP_APPTRACE_DEST_JTAG, rec, sizeof rec, 2000000) != ESP_OK) fails++;
        c = esp_cpu_get_cycle_count() - c;
        if (c < best) best = c;
    }
    esp_apptrace_flush(ESP_APPTRACE_DEST_JTAG, 2000000);
    uint32_t us = (esp_cpu_get_cycle_count() - t0) / 240;
    printf("trace_test: 64 x 1020 B, %lu failed, best write %lu cycles, %lu us = %lu KB/s\n", fails, best, us,
           (uint32_t)(64 * 1024ull * 1000000 / us / 1024));
    for (;;) vTaskDelay(1000);
}
