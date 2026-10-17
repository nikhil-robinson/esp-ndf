#include "t.h"
#include "driver/ledc.h"
void t_ledc(void)
{
    ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                              .timer_num = 0, .freq_hz = 20000, .clk_cfg = LEDC_USE_APB_CLK };
    ledc_timer_config(&t);
    printf("    %-34s %lu Hz\n", "asked 20 kHz/10 bit, got", ledc_get_freq(LEDC_LOW_SPEED_MODE, 0));
    ledc_channel_config_t c = { .gpio_num = 4, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = 0, .timer_sel = 0, .duty = 256 };
    ledc_channel_config(&c);
    METRIC("ledc_duty()", "%lu cycles", CYCLES(ledc_set_duty(LEDC_LOW_SPEED_MODE, 0, 512); ledc_update_duty(LEDC_LOW_SPEED_MODE, 0)));
    t.timer_num = 1; t.freq_hz = 33333; t.duty_resolution = LEDC_TIMER_8_BIT;
    ledc_timer_config(&t);
    printf("    %-34s %lu Hz (integer only)\n", "asked 33333 Hz/8 bit, got", ledc_get_freq(LEDC_LOW_SPEED_MODE, 1));
}
