/* Cross-core interrupt latency by priority level: core 0 raises
 * SYSTEM_CPU_INTR_FROM_CPU_1, core 1's handler answers by writing a word.
 * Round trip in CPU cycles. */
#include "xcore.h"
#include "sys.h"
#include "console.h"

static volatile uint32_t ping, pong, level_req, ready;

static void h(void *a) { (void)a; pong = ping; }

static void core1(void)
{
    uint32_t cur = 0;
    for (;;) {
        if (level_req != cur) { cur = level_req; ready = xc_notify_init(h, 0, cur) == 0 ? cur : 100 + cur; }
    }
}

int main(void)
{
    printf("\n=== cross-core interrupt by level (cycles, 240 = 1 us) ===\n");
    cpu1_start(core1);
    delay_us(10000);
    for (uint32_t lvl = 1; lvl <= 5; lvl += 2) {
        ready = 0; level_req = lvl;
        while (!ready) ;
        if (ready != lvl) { printf("  level %lu: attach failed\n", lvl); continue; }
        uint32_t mn = ~0u, mx = 0, sum = 0;
        for (uint32_t i = 1; i <= 10000; i++) {
            ping = i + lvl * 100000;
            uint32_t t = ccount();
            xc_notify(1);
            while (pong != ping) ;
            t = ccount() - t;
            if (t < mn) mn = t;
            if (t > mx) mx = t;
            sum += t;
        }
        printf("  level %lu: round trip min %lu avg %lu max %lu cycles\n", lvl, mn, sum / 10000, mx);
    }
    printf("=== done ===\n");
    for (;;) ;
}
