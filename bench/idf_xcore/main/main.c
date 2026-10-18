/* IDF sibling of examples/xcore_bench: the same round trips with the tools
 * IDF offers. Two tasks pinned to core 0 / core 1, max priority. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_ipc.h"
#include "esp_cpu.h"

#define N 10000
enum { T_IDLE, T_PLAIN, T_MUX, T_QUEUE, T_MUX_HAMMER, T_QSTREAM, T_DONE };
static volatile uint32_t c1_done;
static volatile uint32_t mode, ping, pong;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t q_to1, q_to0, q_stream;
static volatile uint32_t mux_ctr;

typedef struct { uint32_t min, max; uint64_t sum; } stat_t;
static void st_add(stat_t *s, uint32_t v) { if (v < s->min) s->min = v; if (v > s->max) s->max = v; s->sum += v; }
static void st_print(const char *n, stat_t *s, int k) { printf("  %-40s min %5lu  avg %5lu  max %5lu cycles\n", n, s->min, (uint32_t)(s->sum / k), s->max); }

static void core1_task(void *a)
{
    uint32_t seen = 0;
    for (;;) {
        uint32_t m = mode;
        if (m == T_PLAIN) { uint32_t p = ping; if (p != seen) { seen = p; pong = p; } }
        else if (m == T_MUX) { taskENTER_CRITICAL(&mux); uint32_t p = ping; if (p != seen) { seen = p; pong = p; } taskEXIT_CRITICAL(&mux); }
        else if (m == T_QUEUE) { uint32_t v; if (xQueueReceive(q_to1, &v, portMAX_DELAY)) xQueueSend(q_to0, &v, portMAX_DELAY); }
        else if (m == T_MUX_HAMMER) { for (int i = 0; i < 200000; i++) { taskENTER_CRITICAL(&mux); mux_ctr++; taskEXIT_CRITICAL(&mux); } c1_done = 1; while (mode == T_MUX_HAMMER) ; }
        else if (m == T_QSTREAM) { uint32_t v, n = 0; while (n < 100000) if (xQueueReceive(q_stream, &v, portMAX_DELAY)) n++; c1_done = 1; while (mode == T_QSTREAM) ; }
    }
}

static void ipc_fn(void *arg) { pong = ping; }

static void core0_task(void *a)
{
    vTaskDelay(10);
    printf("\n=== idf_xcore: core 0 <-> core 1, round trips ===\n");
    uint32_t t = esp_cpu_get_cycle_count(); taskENTER_CRITICAL(&mux); taskEXIT_CRITICAL(&mux);
    printf("  portMUX enter+exit critical, uncontended    %lu cycles\n", esp_cpu_get_cycle_count() - t);
    stat_t s;
    mode = T_PLAIN; vTaskDelay(1); s = (stat_t){ ~0u, 0, 0 };
    for (uint32_t i = 1; i <= N; i++) { t = esp_cpu_get_cycle_count(); ping = i; while (pong != i) ; st_add(&s, esp_cpu_get_cycle_count() - t); }
    st_print("plain 32-bit word, round trip", &s, N);
    mode = T_MUX; vTaskDelay(1); s = (stat_t){ ~0u, 0, 0 };
    for (uint32_t i = N + 1; i <= 2 * N; i++) { t = esp_cpu_get_cycle_count(); taskENTER_CRITICAL(&mux); ping = i; taskEXIT_CRITICAL(&mux); while (pong != i) ; st_add(&s, esp_cpu_get_cycle_count() - t); }
    st_print("portMUX-protected word, round trip", &s, N);
    mode = T_QUEUE; vTaskDelay(1); s = (stat_t){ ~0u, 0, 0 };
    for (uint32_t i = 1; i <= N; i++) { uint32_t v; t = esp_cpu_get_cycle_count(); xQueueSend(q_to1, &i, portMAX_DELAY); xQueueReceive(q_to0, &v, portMAX_DELAY); st_add(&s, esp_cpu_get_cycle_count() - t); }
    st_print("FreeRTOS queue, round trip", &s, N);
    mode = T_IDLE; uint32_t z = 0; xQueueSend(q_to1, &z, 0); vTaskDelay(1);
    s = (stat_t){ ~0u, 0, 0 };
    for (uint32_t i = 3 * N; i < 3 * N + 1000; i++) { ping = i; t = esp_cpu_get_cycle_count(); esp_ipc_call_blocking(1, ipc_fn, 0); st_add(&s, esp_cpu_get_cycle_count() - t); }
    st_print("esp_ipc_call_blocking (cross-core call)", &s, 1000);
    /* contended: both cores increment one counter under the portMUX */
    mux_ctr = 0; c1_done = 0; mode = T_MUX_HAMMER;
    t = esp_cpu_get_cycle_count();
    for (int i = 0; i < 200000; i++) { taskENTER_CRITICAL(&mux); mux_ctr++; taskEXIT_CRITICAL(&mux); }
    t = esp_cpu_get_cycle_count() - t;
    while (!c1_done) ;
    mode = T_IDLE;
    printf("  portMUX, both cores hammering: counter %lu (expect 400000), %lu cycles/op on core 0\n", mux_ctr, t / 200000);
    /* queue throughput: 100 000 items core 0 -> core 1 */
    c1_done = 0; mode = T_QSTREAM; vTaskDelay(1);
    t = esp_cpu_get_cycle_count();
    for (uint32_t i = 0; i < 100000; i++) xQueueSend(q_stream, &i, portMAX_DELAY);
    while (!c1_done) ;
    t = esp_cpu_get_cycle_count() - t;
    mode = T_IDLE;
    printf("  FreeRTOS queue stream (256 deep): %lu cycles/item end to end\n", t / 100000);
    printf("=== done ===\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    q_to1 = xQueueCreate(4, 4); q_to0 = xQueueCreate(4, 4); q_stream = xQueueCreate(256, 4);
    xTaskCreatePinnedToCore(core1_task, "c1", 4096, 0, configMAX_PRIORITIES - 2, 0, 1);
    xTaskCreatePinnedToCore(core0_task, "c0", 4096, 0, configMAX_PRIORITIES - 2, 0, 0);
}
