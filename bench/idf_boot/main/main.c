/* IDF sibling of examples/boot_time: reset -> app_main with the same RTC-timer code (bench/boot/rtc_boot.h).
 * Result in RTC slow memory 0x50001000 [4] (us), read back with esptool dump_mem. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../boot/rtc_boot.h"
void app_main(void)
{
    rb_step();
    for (;;) vTaskDelay(1000);
}
