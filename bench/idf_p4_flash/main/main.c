/* IDF sibling of components/drivers/flash/test_apps/flash_p4: erase / program 4 KB and 64 KB, read 4 KB and 64 KB through esp_flash, same scratch area. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_flash.h"
#include "esp_cpu.h"
#define AREA 0xF00000u
static uint8_t w[65536], r[65536];
static uint32_t us(uint32_t t) { return (esp_cpu_get_cycle_count() - t) / 360; }
void app_main(void)
{
    vTaskDelay(300);
    for (size_t i = 0; i < sizeof w; i++) w[i] = i * 31 + (i >> 8);
    uint32_t id = 0, size = 0; esp_flash_read_id(NULL, &id); esp_flash_get_size(NULL, &size);
    printf("idf flash: JEDEC %06lx, %lu MB\n", (unsigned long)id, (unsigned long)(size >> 20));
    uint32_t t = esp_cpu_get_cycle_count(); esp_flash_erase_region(NULL, AREA, 4096); uint32_t e4 = us(t);
    t = esp_cpu_get_cycle_count(); esp_flash_erase_region(NULL, AREA, 65536); uint32_t e64 = us(t);
    t = esp_cpu_get_cycle_count(); esp_flash_write(NULL, w, AREA, 4096); uint32_t w4 = us(t);
    esp_flash_erase_region(NULL, AREA, 65536);
    t = esp_cpu_get_cycle_count(); esp_flash_write(NULL, w, AREA, 65536); uint32_t w64 = us(t);
    uint32_t r4 = ~0u, r64 = ~0u; int bad = 0;
    for (int k = 0; k < 4; k++) {
        memset(r, 0, sizeof r);
        t = esp_cpu_get_cycle_count(); esp_flash_read(NULL, r, AREA, 4096); uint32_t a = us(t); if (a < r4) r4 = a;
        t = esp_cpu_get_cycle_count(); esp_flash_read(NULL, r, AREA, 65536); uint32_t b = us(t); if (b < r64) r64 = b;
    }
    for (size_t i = 0; i < sizeof w; i++) bad += r[i] != w[i];
    printf("idf  erase 4 KB %lu us, 64 KB %lu us | write 4 KB %lu us, 64 KB %lu us | read 4 KB %lu us (%lu MB/s), 64 KB %lu us (%lu MB/s) | %d wrong\n",
           (unsigned long)e4, (unsigned long)e64, (unsigned long)w4, (unsigned long)w64, (unsigned long)r4, (unsigned long)(4096 / r4), (unsigned long)r64, (unsigned long)(65536 / r64), bad);
    esp_flash_erase_region(NULL, AREA, 65536);
    printf("idf_p4_flash done\n");
    for (;;) vTaskDelay(1000);
}
