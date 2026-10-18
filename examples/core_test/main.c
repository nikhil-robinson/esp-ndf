/* Core services: time base, software timers (accuracy, drift, many timers,
 * stop/restart from callbacks, cross-core start), heap, log, delays. */
#include "sys.h"
#include "swtimer.h"
#include "heap.h"
#include "log.h"
#include "xtensa.h"
#include "console.h"
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static volatile uint32_t n_per, n_one, n_self;
static volatile uint64_t t_last, t_first;
static volatile uint32_t lat_min = ~0u, lat_max;
static swtimer_t tp, to, ts, many[32];
static volatile uint32_t many_hits[32];

static void cb_per(void *a)
{
    (void)a;
    uint64_t now = time_ticks();
    uint32_t lat = (uint32_t)(now - tp.when + tp.period);   /* tp.when already advanced */
    if (n_per) { if (lat < lat_min) lat_min = lat; if (lat > lat_max) lat_max = lat; }
    if (!n_per) t_first = now;
    t_last = now;
    n_per++;
}
static void cb_one(void *a) { (void)a; n_one++; }
static void cb_self(void *a) { (void)a; if (++n_self == 5) swt_stop(&ts); }
static void cb_many(void *a) { many_hits[(int)(intptr_t)a]++; }

static volatile int c1_done;
static swtimer_t tc1;
static volatile int c1_hit;
static void cb_c1(void *a) { (void)a; c1_hit = core_id() + 1; }
static void core1(void) { swt_start(&tc1, 1000, false, cb_c1, 0); c1_done = 1; for (;;) {} }

int main(void)
{
    printf("core_test\n");

    /* time base */
    uint64_t a = time_us(); delay_us(10000); uint64_t b = time_us();
    CHECK(b - a >= 10000 && b - a < 10050, "delay_us(10000) = %lu us by SYSTIMER", (unsigned long)(b - a));
    a = time_us(); delay_ms(250); b = time_us();
    CHECK(b - a >= 250000 && b - a < 250200, "delay_ms(250) = %lu us", (unsigned long)(b - a));
    uint32_t c0 = ccount(); for (int i = 0; i < 100; i++) (void)time_ticks(); uint32_t c = (ccount() - c0) / 100;
    CHECK(c < 200, "time_ticks() costs %lu cycles", (unsigned long)c);
    uint64_t prev = time_ticks(); bool mono = true;
    for (int i = 0; i < 100000; i++) { uint64_t t = time_ticks(); if (t < prev) mono = false; prev = t; }
    CHECK(mono, "time_ticks monotonic over 100k reads");

    /* periodic 1 kHz for 1 s: count, drift, latency */
    swt_start(&tp, 1000, true, cb_per, 0);
    delay_ms(1000);
    swt_stop(&tp);
    uint32_t n = n_per;
    uint64_t span = t_last - t_first;
    CHECK(n >= 999 && n <= 1001, "periodic 1 ms: %lu calls in 1 s", (unsigned long)n);
    CHECK((span >= (uint64_t)(n - 1) * 16000 - 16 && span <= (uint64_t)(n - 1) * 16000 + 64),
          "no drift: %lu calls span %lu ticks (ideal %lu)", (unsigned long)n, (unsigned long)span, (unsigned long)(n - 1) * 16000);
    CHECK(lat_max < 64, "callback latency %lu..%lu ticks (%lu..%lu ns)", (unsigned long)lat_min, (unsigned long)lat_max,
          (unsigned long)lat_min * 625 / 10, (unsigned long)lat_max * 625 / 10);
    uint32_t after = n_per; delay_ms(20);
    CHECK(n_per == after, "stopped timer stays stopped");

    /* one-shot fires once */
    swt_start(&to, 500, false, cb_one, 0); delay_ms(5);
    CHECK(n_one == 1 && !swt_active(&to), "one-shot fired once");

    /* stop itself from its callback */
    swt_start(&ts, 200, true, cb_self, 0); delay_ms(10);
    CHECK(n_self == 5 && !swt_active(&ts), "periodic stopped from its own callback after 5");

    /* 32 timers, mixed periods 100..3200 us, run 100 ms */
    for (int i = 0; i < 32; i++) swt_start(&many[i], 100 * (i + 1), true, cb_many, (void *)(intptr_t)i);
    delay_ms(100);
    for (int i = 0; i < 32; i++) swt_stop(&many[i]);
    int bad = 0;
    for (int i = 0; i < 32; i++) { uint32_t want = 100000 / (100 * (i + 1)); if (many_hits[i] + 1 < want || many_hits[i] > want + 1) bad++; }
    CHECK(!bad, "32 concurrent timers: counts within +-1 (%d off)", bad);

    /* start from core 1: callback runs on core 0 (owner of the ISR) */
    cpu1_start(core1);
    while (!c1_done) {}
    delay_ms(3);
    CHECK(c1_hit == 1, "timer started on core 1 fired (core %d)", c1_hit - 1);

    /* heap */
    size_t f0 = heap_free();
    void *p[64];
    for (int i = 0; i < 64; i++) { p[i] = malloc(17 + i * 33); if (p[i]) memset(p[i], i, 17 + i * 33); }
    int al = 0; for (int i = 0; i < 64; i++) if (!p[i] || ((uintptr_t)p[i] & 15)) al++;
    CHECK(!al, "64 mallocs ok, all 16-byte aligned");
    int corrupt = 0;
    for (int i = 0; i < 64; i += 2) free(p[i]);
    for (int i = 1; i < 64; i += 2) for (int k = 0; k < 17 + i * 33; k++) if (((uint8_t *)p[i])[k] != i) { corrupt++; break; }
    CHECK(!corrupt, "odd blocks intact after freeing even ones");
    for (int i = 1; i < 64; i += 2) free(p[i]);
    CHECK(heap_free() == f0 && heap_largest() == f0, "all freed and merged back: %lu bytes", (unsigned long)f0);
    void *big = malloc(f0); CHECK(big != 0, "whole heap in one block"); free(big);
    CHECK(malloc(f0 + 16) == 0, "oversize malloc returns NULL");
    uint8_t *r = malloc(10); for (int i = 0; i < 10; i++) r[i] = i; r = realloc(r, 1000);
    int ok = 1; for (int i = 0; i < 10; i++) if (r[i] != i) ok = 0; free(r);
    uint32_t *z = calloc(100, 4); for (int i = 0; i < 100; i++) if (z[i]) ok = 0; free(z);
    CHECK(ok && heap_free() == f0, "realloc keeps data, calloc zeroes");
    c0 = ccount(); for (int i = 0; i < 100; i++) free(malloc(64)); c = (ccount() - c0) / 100;
    printf("     malloc+free 64 B: %lu cycles\n", (unsigned long)c);

    LOGI("core", "log line, heap min free %lu", (unsigned long)heap_min_free());
    printf("=== %s (%d failures) ===\n", fails ? "FAILED" : "ALL PASS", fails);
    for (;;) {}
}
