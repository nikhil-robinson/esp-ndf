/* IDF sibling of components/drivers/flash/test_apps/flash_test: the same erase / write / read measurements through esp_flash, and flash code on
 * core 1 (cache miss forced on every call) during a 64 KB erase + 4 KB write. Built twice: default (IDF disables the
 * cache meanwhile: core 1 waits) and with sdkconfig.suspend (auto-suspend). SPI0/SPI1 registers after init go to RTC
 * memory 0x50001400 (read back with esptool dump_mem) as the reference for esp-ndf. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_flash.h"
#include "spi_flash_chip_driver.h"
#include "esp_cpu.h"
#include "esp_attr.h"
#include "esp32s3/rom/cache.h"

#define AREA (16u * 1024 * 1024 - 0x100000)
static uint8_t w[65536], r[65536];
static uint32_t us_since(uint32_t t) { return (esp_cpu_get_cycle_count() - t) / 240; }

#define KERNEL { for (int k = 0; k < 16; k++) x = x * 1664525u + 1013904223u; return x; }
static __attribute__((noipa)) uint32_t flash_kernel(uint32_t x) KERNEL           /* in flash (IDF default) */
static IRAM_ATTR __attribute__((noipa)) uint32_t ram_kernel(uint32_t x) KERNEL

static volatile uint32_t c1_runs, c1_bad, c1_gap, c1_go, c1_every;
static void IRAM_ATTR core1_task(void *arg)
{
    uint32_t last = esp_cpu_get_cycle_count();
    for (;;) {
        if (!c1_go) { last = esp_cpu_get_cycle_count(); continue; }
        Cache_Invalidate_Addr((uint32_t)flash_kernel & ~31u, 64);           /* next call fetches from flash */
        uint32_t x = c1_runs;
        if (flash_kernel(x) != ram_kernel(x)) c1_bad++;
        uint32_t now = esp_cpu_get_cycle_count();
        if (now - last > c1_gap) c1_gap = now - last;
        c1_runs++;
        if (c1_every) while (esp_cpu_get_cycle_count() - now < c1_every * 240) {}
        last = esp_cpu_get_cycle_count();
    }
}
static int check(const uint8_t *a, uint8_t v, size_t n) { for (size_t i = 0; i < n; i++) if (a[i] != v) return 0; return 1; }

void app_main(void)
{
    volatile uint32_t *rd = (volatile uint32_t *)0x50001400u;
    for (int k = 0; k < 64; k++) { rd[k] = *(volatile uint32_t *)(0x60002000u + 4 * k); rd[64 + k] = *(volatile uint32_t *)(0x60003000u + 4 * k); }
    vTaskDelay(300);
    uint32_t id = 0; esp_flash_read_id(NULL, &id);
    uint64_t uid = 0; esp_flash_read_unique_chip_id(NULL, &uid);
    printf("idf flash: JEDEC %06lx, chip driver %s, unique ID %016llx\n", (unsigned long)id, esp_flash_default_chip->chip_drv->name, uid);
    uint32_t a = AREA, t;
    t = esp_cpu_get_cycle_count(); esp_flash_erase_region(NULL, a, 4096); uint32_t te4 = us_since(t);
    t = esp_cpu_get_cycle_count(); esp_flash_erase_region(NULL, a + 0x8000, 0x8000); uint32_t te32 = us_since(t);
    t = esp_cpu_get_cycle_count(); esp_flash_erase_region(NULL, a + 0x10000, 0x10000); uint32_t te64 = us_since(t);
    t = esp_cpu_get_cycle_count(); esp_flash_erase_region(NULL, a + 0x40000, 0x40000); uint32_t te256 = us_since(t);
    esp_flash_read(NULL, r, a, 4096); int ok4 = check(r, 0xFF, 4096);
    esp_flash_read(NULL, r, a + 0x10000, 65536); int ok64 = check(r, 0xFF, 65536);
    printf("idf erase: 4 KB %lu us, 32 KB %lu us, 64 KB %lu us, 256 KB region %lu us; blank %d/%d\n",
           (unsigned long)te4, (unsigned long)te32, (unsigned long)te64, (unsigned long)te256, ok4, ok64);
    for (size_t i = 0; i < sizeof w; i++) w[i] = (uint8_t)(i * 31 + (i >> 8));
    t = esp_cpu_get_cycle_count(); esp_flash_write(NULL, w, a, 4096); uint32_t tw4 = us_since(t);
    t = esp_cpu_get_cycle_count(); esp_flash_write(NULL, w, a + 0x10000, 65536); uint32_t tw64 = us_since(t);
    t = esp_cpu_get_cycle_count(); esp_flash_read(NULL, r, a, 4096); uint32_t tr4 = us_since(t);
    int v4 = !memcmp(r, w, 4096);
    t = esp_cpu_get_cycle_count(); esp_flash_read(NULL, r, a + 0x10000, 65536); uint32_t tr64 = us_since(t);
    int v64 = !memcmp(r, w, 65536);
    printf("idf write: 4 KB %lu us, 64 KB %lu us; read: 4 KB %lu us, 64 KB %lu us; data %s\n", (unsigned long)tw4,
           (unsigned long)tw64, (unsigned long)tr4, (unsigned long)tr64, v4 && v64 ? "ok" : "wrong");

    xTaskCreatePinnedToCore(core1_task, "c1", 4096, NULL, 24, NULL, 1);
    vTaskDelay(20); c1_go = 1; vTaskDelay(20);
    static const uint32_t every[] = { 0, 20, 50, 100, 200, 500, 1000 };
    for (unsigned m = 0; m < sizeof every / sizeof every[0]; m++) {
        c1_every = every[m]; vTaskDelay(5);
        uint32_t before = c1_runs; c1_gap = 0;
        t = esp_cpu_get_cycle_count();
        esp_err_t ee = esp_flash_erase_region(NULL, a + 0x20000, 0x10000), ww = esp_flash_write(NULL, w, a + 0x20000, 4096);
        uint32_t top = us_since(t), runs = c1_runs - before, gap = c1_gap / 240;
        esp_flash_read(NULL, r, a + 0x20000, 4096); int vv = !memcmp(r, w, 4096);
        esp_flash_read(NULL, r, a + 0x21000, 4096); int bl = check(r, 0xFF, 4096);
        printf("idf miss every %4lu us: erase 64 KB + write 4 KB %7lu us; core 1 %5lu runs, longest pause %5lu us, "
               "bad %lu, data %s, err %d/%d\n", (unsigned long)every[m], (unsigned long)top, (unsigned long)runs,
               (unsigned long)gap, (unsigned long)c1_bad, vv && bl ? "ok" : "wrong", ee, ww);
    }
    c1_go = 0;
    esp_flash_erase_region(NULL, a, 0x80000);
    printf("idf_flash done\n");
    for (;;) vTaskDelay(1000);
}
