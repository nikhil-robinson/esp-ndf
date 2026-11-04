/* Ring buffer, async DMA copy, watchdog and panic report. Runs over three
 * boots, chosen by the reset cause:
 *   1st (any other cause): ringbuf + async DMA + watchdog fed, then stall ->
 *       watchdog report + reset
 *   2nd (TG0 watchdog):    check the cause, then fault (store to 0) ->
 *       panic report + restart (NDF_PANIC_RESTART)
 *   3rd (software reset):  check the cause, done. */
#include "sys.h"
#include "system.h"
#include "ringbuf.h"
#include "gdma.h"
#include "wdt.h"
#include "console.h"
#include "xtensa.h"
#include "xcore.h"
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static uint8_t rbmem[1024] __attribute__((aligned(4)));
static ringbuf_t rb;
static uint8_t big[2][1 << 16] __attribute__((aligned(4)));   /* 64 KB src + dst for DMA */

/* core 1 producer: items of length 1..200 with a sequence pattern */
static volatile uint32_t produced, prod_full;
#define NITEMS 20000
static void producer(void)
{
    uint8_t tmp[200];
    for (uint32_t i = 0; i < NITEMS; i++) {
        uint32_t len = 1 + (i * 7919) % 200;
        for (uint32_t k = 0; k < len; k++) tmp[k] = (uint8_t)(i + k);
        while (!rb_put(&rb, tmp, len)) prod_full++;
        produced = i + 1;
    }
    for (;;) {}
}

static volatile int dma_done; static volatile uint32_t dma_t;
static void on_dma(void *a) { (void)a; dma_t = ccount(); dma_done = 1; }

static void stage1(void)
{
    /* ring buffer: basics */
    rb_init(&rb, rbmem, sizeof rbmem);
    uint8_t out[600]; uint32_t len;
    CHECK(rb_get(&rb, out, sizeof out) == -1, "empty ring returns -1");
    CHECK(rb_put(&rb, "hello", 5) && rb_get(&rb, out, sizeof out) == 5 && !memcmp(out, "hello", 5), "put/get one item");
    int n = 0; while (rb_put(&rb, "0123456789abcdef0123456789abcdef", 32)) n++;
    CHECK(n == 1024 / 36 - 1 || n == 1024 / 36, "fills to capacity: %d x 36 B items in 1024 B", n);
    int got = 0; while (rb_get(&rb, out, sizeof out) == 32) got++;
    CHECK(got == n, "drains all %d", got);
    uint8_t *w = rb_reserve(&rb, 500); if (w) { memset(w, 0xA5, 500); rb_commit(&rb); }
    uint8_t *r = rb_peek(&rb, &len);
    CHECK(w && r == w && len == 500, "zero-copy reserve/commit/peek: same buffer, wraps around");
    CHECK(rb_get(&rb, out, 100) == -2, "item larger than out buffer stays queued");
    rb_release(&rb);
    CHECK(rb_used(&rb) == 0, "released, empty");

    /* timing: put + get of 16 B, same core */
    uint8_t m16[16] = { 1 };
    uint32_t c0 = ccount(); for (int i = 0; i < 100; i++) { rb_put(&rb, m16, 16); rb_get(&rb, out, 16); } uint32_t pc = (ccount() - c0) / 100;
    c0 = ccount(); for (int i = 0; i < 100; i++) { uint8_t *p = rb_reserve(&rb, 16); p[0] = 1; rb_commit(&rb); rb_peek(&rb, &len); rb_release(&rb); } uint32_t zc = (ccount() - c0) / 100;
    printf("     ringbuf 16 B put+get %lu cycles, zero-copy reserve+commit+peek+release %lu cycles\n", (unsigned long)pc, (unsigned long)zc);

    /* core 1 -> core 0 stream of 20000 variable items through 1 KB */
    rb_init(&rb, rbmem, sizeof rbmem);
    cpu1_start(producer);
    uint32_t bad = 0, i = 0, t0 = ccount();
    while (i < NITEMS) {
        uint8_t *p = rb_peek(&rb, &len);
        if (!p) continue;
        uint32_t want = 1 + (i * 7919) % 200;
        if (len != want) bad++;
        else for (uint32_t k = 0; k < len; k++) if (p[k] != (uint8_t)(i + k)) { bad++; break; }
        rb_release(&rb); i++;
    }
    uint32_t cyc = ccount() - t0;
    CHECK(!bad, "core1 -> core0: %d variable items (1..200 B) intact, %lu cycles/item, producer waited %lu times",
          NITEMS, (unsigned long)cyc / NITEMS, (unsigned long)prod_full);

    /* async DMA copy: CPU keeps working */
    int ch = gdma_alloc(); gdma_connect(ch, GDMA_M2M);
    for (int k = 0; k < 65472; k++) big[0][k] = (uint8_t)(k * 13 + 7);
    memset(big[1], 0, sizeof big[1]);
    c0 = ccount();
    int rc = gdma_memcpy_async(ch, big[1], big[0], 65472, on_dma, 0, 1);
    uint32_t start_c = ccount() - c0;
    volatile uint32_t work = 0; while (rc == 0 && !dma_done) work++;      /* CPU is free meanwhile (a refused start must fail the test, not hang it) */
    uint32_t total = dma_t - c0;
    CHECK(rc == 0 && !memcmp(big[0], big[1], 65472), "async DMA 65472 B: correct; start returns in %lu cycles", (unsigned long)start_c);
    printf("     done callback after %lu cycles = %lu MB/s; CPU looped %lu times meanwhile\n",
           (unsigned long)total, (unsigned long)(65472ull * 240 / total), (unsigned long)work);
    CHECK(!gdma_busy(ch), "gdma_busy false after callback");
    memset(big[1], 0, 4096);
    gdma_memcpy_async(ch, big[1], big[0], 4096, 0, 0, 0);
    while (gdma_busy(ch)) {}
    CHECK(!memcmp(big[0], big[1], 4096), "async DMA polled with gdma_busy");

    /* watchdog fed: no reset */
    CHECK(wdt_start(100, true) == 0, "watchdog 100 ms started");
    c0 = ccount(); wdt_feed(); uint32_t fc = ccount() - c0;
    for (int k = 0; k < 50; k++) { delay_ms(10); wdt_feed(); }
    CHECK(1, "fed every 10 ms for 500 ms: still running (feed %lu cycles)", (unsigned long)fc);
    printf("stage1 done: stalling now, expect a watchdog report then reset\n");
    for (;;) {}                                                  /* not fed: report at 100 ms, reset at 200 ms */
}

int main(void)
{
    int cause = sys_reset_cause(0);
    printf("sys_test: reset cause %s\n", sys_reset_cause_str(cause));
    if (cause == RESET_TG0WDT_SYS) {
        CHECK(1, "came back from the watchdog reset");
        printf("stage2: fault on purpose (store to 0), expect a panic report then restart\n");
        delay_ms(5);
        *(volatile uint32_t *)0 = 0x1234;
    } else if (cause == RESET_SW_SYS) {
        CHECK(1, "came back from the panic restart");
        printf("=== %s (%d failures) ===\n", fails ? "FAILED" : "ALL PASS", fails);
    } else stage1();
    for (;;) {}
}
