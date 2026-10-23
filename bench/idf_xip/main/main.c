/* IDF sibling of examples/xip_test: the same generated kernels (bench/xip/xip_kern.c) in IRAM and in flash (IDF's
 * default placement), cycles per call from IRAM, flash warm, flash cold (ROM Cache_Invalidate_ICache_All first);
 * flash erase + write time. Results into RTC slow memory 0x50001000 (read back with esptool dump_mem). */
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_flash.h"
#include "esp32s3/rom/cache.h"

#define XK_ATTR IRAM_ATTR
#define XK_P    xr
#include "../../xip/xip_kern.c"
#undef XK_ATTR
#undef XK_P
#define XK_ATTR __attribute__((noinline))
#define XK_P    xf
#include "../../xip/xip_kern.c"

static uint32_t pass(uint32_t (*const t[])(uint32_t), uint32_t *sum)
{
    uint32_t s = 0, c0 = esp_cpu_get_cycle_count();
    for (int k = 0; k < XK_COUNT; k++) s += t[k](s + (uint32_t)k);
    uint32_t c = esp_cpu_get_cycle_count() - c0;
    *sum = s;
    return c;
}

void app_main(void)
{
    volatile uint32_t *R = (volatile uint32_t *)0x50001000u;
    vTaskDelay(10);
    uint32_t sr, sf; pass(xr_table, &sr); pass(xf_table, &sf);
    uint32_t ram = pass(xr_table, &sr), warm = pass(xf_table, &sf);
    Cache_Invalidate_ICache_All();
    uint32_t cold = pass(xf_table, &sf);
    static uint8_t w[256];
    for (int k = 0; k < 256; k++) w[k] = (uint8_t)(k * 7 + 3);
    uint32_t a = 0x3FF000, t0 = esp_cpu_get_cycle_count();
    esp_err_t e = esp_flash_erase_region(NULL, a, 0x1000), wr = esp_flash_write(NULL, w, a, sizeof w);
    uint32_t us = (esp_cpu_get_cycle_count() - t0) / 240;
    R[1] = sr == sf; R[2] = ram / XK_COUNT; R[3] = warm / XK_COUNT; R[4] = cold / XK_COUNT; R[5] = us; R[6] = (uint32_t)(e | wr);
    R[0] = 0x49584950;                                   /* "PIXI" */
    for (;;) vTaskDelay(1000);
}
