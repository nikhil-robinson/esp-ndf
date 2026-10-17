#include "t.h"
#include "driver/touch_pad.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static uint32_t avg(int pad) { uint32_t s = 0, v; for (int i = 0; i < 16; i++) { touch_pad_read_raw_data(pad, &v); s += v; vTaskDelay(1); } return s / 16; }
void t_touch(void)
{
    gpio_reset_pin(11); gpio_set_direction(11, GPIO_MODE_INPUT);
    touch_pad_init();
    touch_pad_config(1); touch_pad_config(10); touch_pad_config(14);
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER); touch_pad_fsm_start();
    vTaskDelay(5);
    uint32_t p1 = avg(1), p10 = avg(10), p14 = avg(14);
    printf("    raw: pad1 %lu, pad14 %lu, pad10 %lu (jumper)\n", p1, p14, p10);
    uint32_t v;
    METRIC("touch_read()", "%lu cycles", CYCLES(touch_pad_read_raw_data(10, &v)));
    touch_pad_fsm_stop(); touch_pad_deinit(); gpio_reset_pin(11);
}
