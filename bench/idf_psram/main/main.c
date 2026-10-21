/* IDF sibling of examples/psram_test: octal PSRAM at 80 MHz, memcpy 1 MiB SRAM <-> PSRAM, CPU cycle counter, 240 MHz */
#include <stdio.h>
#include <string.h>
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
static uint32_t sram_buf[8192];
void app_main(void)
{
    uint8_t *p = heap_caps_malloc(1 << 20, MALLOC_CAP_SPIRAM);
    printf("psram %u, buf %p\n", (unsigned)esp_psram_get_size(), p);
    uint32_t t0 = esp_cpu_get_cycle_count(); for (int r = 0; r < 32; r++) memcpy(p + r * sizeof sram_buf, sram_buf, sizeof sram_buf);
    uint32_t t1 = esp_cpu_get_cycle_count();
    uint32_t t2 = esp_cpu_get_cycle_count(); for (int r = 0; r < 32; r++) memcpy(sram_buf, p + r * sizeof sram_buf, sizeof sram_buf);
    uint32_t t3 = esp_cpu_get_cycle_count();
    printf("IDF memcpy SRAM->PSRAM %.1f MB/s, PSRAM->SRAM %.1f MB/s (1 MiB, cache write-back not forced)\n", (1 << 20) / ((t1 - t0) / 240.0), (1 << 20) / ((t3 - t2) / 240.0));
    printf("=== done\n");
}
