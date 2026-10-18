#include "t.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile uint32_t n; static volatile int64_t t0, tl; static volatile int32_t jmin = 1 << 30, jmax = -(1 << 30);
static void cb(void *a)
{
    int64_t now = esp_timer_get_time();
    if (n) { int32_t d = (int32_t)(now - tl); if (d < jmin) jmin = d; if (d > jmax) jmax = d; } else t0 = now;
    tl = now; n++;
}

void t_core(void)
{
    METRIC("esp_timer_get_time()", "%lu cycles", CYCLES((void)esp_timer_get_time()));
    int64_t a = esp_timer_get_time(); esp_rom_delay_us(10000); int64_t b = esp_timer_get_time();
    METRIC("esp_rom_delay_us(10000)", "%lu us", (uint32_t)(b - a));
    a = esp_timer_get_time(); vTaskDelay(pdMS_TO_TICKS(250)); b = esp_timer_get_time();
    METRIC("vTaskDelay(250 ms)", "%lu us", (uint32_t)(b - a));
    esp_timer_handle_t h; esp_timer_create_args_t c = { .callback = cb, .name = "p" };
    esp_timer_create(&c, &h);
    esp_timer_start_periodic(h, 1000);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_timer_stop(h);
    METRIC("esp_timer 1 ms: calls in 1 s", "%lu", n);
    METRIC("esp_timer period min..max", "%ld..%ld us", jmin, jmax);
    METRIC("esp_timer span drift", "%ld us", (int32_t)(tl - t0 - (int64_t)(n - 1) * 1000));
    void *volatile vp; uint32_t cy = CYCLES(vp = malloc(64); free(vp));
    METRIC("malloc+free 64 B", "%lu cycles", cy);
    METRIC("heap free (internal)", "%u bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
