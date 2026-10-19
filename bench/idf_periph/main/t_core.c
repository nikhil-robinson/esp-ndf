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

#include <math.h>
#include "freertos/ringbuf.h"
#include "esp_async_memcpy.h"
#include "esp_task_wdt.h"
#include "esp_private/esp_clk.h"

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static volatile int am_done; static volatile uint32_t am_t;
static bool IRAM_ATTR am_cb(async_memcpy_handle_t h, async_memcpy_event_t *e, void *a) { am_t = ccount(); am_done = 1; return false; }

void t_core2(void)
{
    volatile float fa = 1.2345f, fb = 6.789f; float r = 0;
    uint32_t c0 = ccount(); for (int i = 0; i < 100; i++) r += fa / fb; METRIC("float divide", "%lu cycles", (ccount() - c0) / 100);
    c0 = ccount(); for (int i = 0; i < 100; i++) r += sqrtf(fb); METRIC("sqrtf", "%lu cycles", (ccount() - c0) / 100);
    char buf[64];
    c0 = ccount(); for (int i = 0; i < 10; i++) snprintf(buf, sizeof buf, "%.3f", 3.14159); METRIC("snprintf %.3f", "%lu cycles", (ccount() - c0) / 10);
    c0 = ccount(); for (int i = 0; i < 10; i++) snprintf(buf, sizeof buf, "%d", 123456); METRIC("snprintf %d", "%lu cycles", (ccount() - c0) / 10);
    c0 = ccount(); for (int i = 0; i < 10; i++) (void)strtof("3.14159", 0); METRIC("strtof", "%lu cycles (r=%d)", (ccount() - c0) / 10, (int)r);
    static int arr[2000]; uint32_t x = 1;
    for (int i = 0; i < 2000; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; arr[i] = (int)(x % 100000) - 50000; }
    c0 = ccount(); qsort(arr, 2000, sizeof(int), cmp_int); METRIC("qsort 2000 ints", "%lu cycles", ccount() - c0);

    RingbufHandle_t rb = xRingbufferCreate(1024, RINGBUF_TYPE_NOSPLIT);
    uint8_t m16[16] = { 1 }; size_t len;
    c0 = ccount();
    for (int i = 0; i < 100; i++) { xRingbufferSend(rb, m16, 16, 0); void *p = xRingbufferReceive(rb, &len, 0); vRingbufferReturnItem(rb, p); }
    METRIC("ringbuf 16 B send+receive+return", "%lu cycles", (ccount() - c0) / 100);
    c0 = ccount();
    for (int i = 0; i < 100; i++) { void *w; xRingbufferSendAcquire(rb, &w, 16, 0); ((uint8_t *)w)[0] = 1; xRingbufferSendComplete(rb, w); void *p = xRingbufferReceive(rb, &len, 0); vRingbufferReturnItem(rb, p); }
    METRIC("ringbuf zero-copy acquire+complete+recv+return", "%lu cycles", (ccount() - c0) / 100);

    async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    async_memcpy_handle_t am; esp_async_memcpy_install(&cfg, &am);
    uint8_t *src = heap_caps_malloc(65472, MALLOC_CAP_DMA), *dst = heap_caps_malloc(65472, MALLOC_CAP_DMA);
    for (int i = 0; i < 65472; i++) src[i] = (uint8_t)(i * 13 + 7);
    c0 = ccount();
    esp_err_t e = esp_async_memcpy(am, dst, src, 65472, am_cb, 0);
    uint32_t st = ccount() - c0;
    while (!am_done) {}
    uint32_t tot = am_t - c0;
    METRIC("async memcpy 65472 B: start", "%lu cycles (%s, data %s)", st, esp_err_to_name(e), memcmp(src, dst, 65472) ? "BAD" : "ok");
    METRIC("async memcpy done after", "%lu cycles = %lu MB/s", tot, (uint32_t)(65472ull * 240 / tot));

    esp_task_wdt_config_t wc = { .timeout_ms = 1000, .idle_core_mask = 0, .trigger_panic = false };
    if (esp_task_wdt_init(&wc) != ESP_OK) esp_task_wdt_reconfigure(&wc);
    esp_task_wdt_add(NULL);
    METRIC("esp_task_wdt_reset()", "%lu cycles", CYCLES(esp_task_wdt_reset()));
    esp_task_wdt_delete(NULL);
}

static inline uint32_t dcc(void) { return ccount(); }
#include "dbench.inc"

static inline uint32_t mcc(void) { return ccount(); }
#include "mbench.inc"

void t_core3(void)
{
    dbench();
    mbench();
    volatile double x = 1.2345678901234, y = 6.789012345678; double r = 0;
    uint32_t c0 = ccount(); for (int i = 0; i < 100; i++) r += x * y; uint32_t cm = (ccount() - c0) / 100;
    c0 = ccount(); for (int i = 0; i < 100; i++) r += x / y; uint32_t cd = (ccount() - c0) / 100;
    c0 = ccount(); for (int i = 0; i < 100; i++) r = r + x; uint32_t ca = (ccount() - c0) / 100;
    METRIC("double add / mul / div", "%lu / %lu / %lu cycles (r=%d)", ca, cm - ca, cd - ca, (int)r);
    char buf[64];
    c0 = ccount(); for (int i = 0; i < 10; i++) snprintf(buf, sizeof buf, "%.17g", 1.2345678901234567e300); METRIC("snprintf %.17g 1.23e300", "%lu cycles", (ccount() - c0) / 10);
    c0 = ccount(); for (int i = 0; i < 10; i++) snprintf(buf, sizeof buf, "%.17g", 4.9406564584124654e-324); METRIC("snprintf %.17g denormal", "%lu cycles", (ccount() - c0) / 10);
    c0 = ccount(); for (int i = 0; i < 10; i++) (void)strtod("3.141592653589793", 0); METRIC("strtod 16 digits", "%lu cycles", (ccount() - c0) / 10);
    c0 = ccount(); for (int i = 0; i < 10; i++) (void)strtod("2.2250738585072011e-308", 0); METRIC("strtod hard case", "%lu cycles", (ccount() - c0) / 10);
}
