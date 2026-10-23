/* Reference for esp-ndf XIP bring-up: IDF runs from flash; copy the cache controller (EXTMEM 0x600C4000..+0x200) and
 * the permission block (0x600C1000..+0x100) registers into RTC slow memory (0x50001000), magic last. Read back with
 * esptool read_mem / dump_mem (RTC memory survives the reset esptool does). */
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
void app_main(void)
{
    volatile uint32_t *r = (volatile uint32_t *)0x50001000u;
    for (int k = 0; k < 128; k++) r[4 + k] = *(volatile uint32_t *)(0x600C4000u + 4 * k);
    for (int k = 0; k < 64; k++) r[4 + 128 + k] = *(volatile uint32_t *)(0x600C1000u + 4 * k);
    r[1] = *(volatile uint32_t *)0x600C5000u; r[2] = *(volatile uint32_t *)(0x600C5000u + 4 * 1);
    r[0] = 0x58495052;                                    /* "XIPR" */
    for (;;) vTaskDelay(1000);
}
