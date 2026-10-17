/* Wiring probe on official ESP-IDF drivers (independent of esp-ndf).
 * 1) shorts: drive each pin high (others pulled down) and low (others pulled
 *    up); report every pin that follows in both cases.
 * 2) analyzer map: LEDC outputs a distinct frequency on 8 pins, forever. */
#include <stdio.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

static const int pins[] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,
                            33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48 };
#define N (int)(sizeof pins / sizeof pins[0])

static void all_inputs(bool pullup)
{
    for (int i = 0; i < N; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], pullup ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
    }
}

void app_main(void)
{
    vTaskDelay(20);
    printf("\n=== wiring probe (ESP-IDF) ===\n");
    for (int i = 0; i < N; i++) {
        uint64_t follow = ~0ull;
        for (int pass = 0; pass < 2; pass++) {
            bool high = pass == 0;
            all_inputs(!high);
            gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
            gpio_set_level(pins[i], high);
            esp_rom_delay_us(200);
            uint64_t m = 0;
            for (int j = 0; j < N; j++)
                if (j != i && gpio_get_level(pins[j]) == high) m |= 1ull << j;
            follow &= m;
        }
        for (int j = 0; j < N; j++)
            if (follow & (1ull << j)) printf("SHORT GPIO%d <-> GPIO%d\n", pins[i], pins[j]);
    }
    all_inputs(false);
    printf("=== shorts done ===\n");

    static const int lp[8] = { 4, 5, 6, 7, 10, 12, 15, 16 };
    /* 4 timers -> 8 frequencies: timer k at (k+1) kHz; channel c uses timer c%4, and
     * channels 4..7 run at 50 % / 25 % duty to tell them apart from 0..3 */
    for (int k = 0; k < 4; k++) {
        ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                                  .timer_num = k, .freq_hz = 1000 * (k + 1), .clk_cfg = LEDC_USE_APB_CLK };
        ledc_timer_config(&t);
    }
    for (int c = 0; c < 8; c++) {
        ledc_channel_config_t ch = { .gpio_num = lp[c], .speed_mode = LEDC_LOW_SPEED_MODE, .channel = c,
                                     .timer_sel = c % 4, .duty = c < 4 ? 128 : 64 };
        ledc_channel_config(&ch);
        printf("LEDC GPIO%d = %d kHz, duty %d%%\n", lp[c], c % 4 + 1, c < 4 ? 50 : 25);
    }
    printf("=== analyzer pattern running ===\n");
}
