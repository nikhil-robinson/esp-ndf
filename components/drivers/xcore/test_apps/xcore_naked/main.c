/* Cross-core interrupt with a naked level-5 handler on core 1 vs C handler. */
#include "xcore.h"
#include "irq.h"
#include "sys.h"
#include "console.h"

volatile uint32_t xcn[5];                       /* a2, a3 save, mailbox in, mailbox out, count */
extern void naked_xc1(void);
static volatile int ready;

static void core1(void)
{
    ready = irq_naked(80, 5, naked_xc1) >= 0 ? 1 : -1;  /* source 80 = CPU_INTR_FROM_CPU_1 */
    for (;;) ;
}

int main(void)
{
    printf("\n=== cross-core interrupt, naked level-5 handler on core 1 (cycles) ===\n");
    cpu1_start(core1);
    while (!ready) ;
    if (ready < 0) { printf("  attach failed\n"); for (;;) ; }
    uint32_t mn = ~0u, mx = 0, sum = 0;
    for (uint32_t i = 1; i <= 10000; i++) {
        xcn[2] = i;
        uint32_t t = ccount();
        xc_notify(1);
        while (xcn[3] != i) ;
        t = ccount() - t;
        if (t < mn) mn = t;
        if (t > mx) mx = t;
        sum += t;
    }
    printf("  naked:  round trip min %lu avg %lu max %lu, handler ran %lu times for 10000 notifications\n", mn, sum / 10000, mx, xcn[4]);
    printf("  (C handler level 5: 96; polling a word: 30-49)\n=== done ===\n");
    for (;;) ;
}
