/* IDF sibling of examples/runtime_test: esp_event post + dispatch, FreeRTOS task switch (taskYIELD ping-pong). */
#include <stdio.h>
#include "esp_event.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

ESP_EVENT_DEFINE_BASE(BENCH);
static volatile uint32_t got, t_post;
static void h(void *a, esp_event_base_t b, int32_t id, void *d) { got = esp_cpu_get_cycle_count() - t_post; }

static volatile uint32_t mark, best = ~0u; static volatile int runs;
static void ta(void *p) { for (int i = 0; i < 64; i++) { mark = esp_cpu_get_cycle_count(); taskYIELD(); } vTaskDelete(NULL); }
static void tb(void *p) { for (int i = 0; i < 64; i++) { uint32_t d = esp_cpu_get_cycle_count() - mark; if (i && d < best) best = d; taskYIELD(); } runs = 1; vTaskDelete(NULL); }

void app_main(void)
{
    esp_event_loop_args_t la = { .queue_size = 16, .task_name = NULL };    /* no task: dispatched by run_to, like ours */
    esp_event_loop_handle_t loop;
    esp_event_loop_create(&la, &loop);
    esp_event_handler_register_with(loop, BENCH, 1, h, NULL);
    uint32_t bp = ~0u, bd = ~0u;
    for (int i = 0; i < 16; i++) {
        uint32_t c = esp_cpu_get_cycle_count(); esp_event_post_to(loop, BENCH, 1, NULL, 0, 0); uint32_t p = esp_cpu_get_cycle_count() - c;
        c = esp_cpu_get_cycle_count(); esp_event_loop_run(loop, 0); uint32_t d = esp_cpu_get_cycle_count() - c;
        if (p < bp) bp = p;
        if (d < bd) bd = d;
    }
    printf("idf_runtime: esp_event post %lu cycles, run_to dispatch %lu cycles\n", bp, bd);
    xTaskCreatePinnedToCore(ta, "a", 2048, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(tb, "b", 2048, NULL, 5, NULL, 0);
    while (!runs) vTaskDelay(1);
    printf("idf_runtime: taskYIELD task -> task switch %lu cycles\n", best);
    printf("idf_runtime done\n");
}
