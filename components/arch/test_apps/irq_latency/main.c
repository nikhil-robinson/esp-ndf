/* Interrupt latency in CPU cycles, per level, using the core's CCOMPARE
 * timers: they fire at an exact CCOUNT, so latency = CCOUNT at handler
 * entry - CCOMPARE. Measures our whole path: vector + context save + C
 * dispatch + call. Compare: Cortex-M4 = 12 cycles to first handler insn. */
#include "sys.h"
#include "irq.h"
#include "console.h"
#include "xtensa.h"

#define N 20000

typedef struct { uint32_t min, max, n; uint64_t sum; volatile uint32_t target, done; } lat_t;
static lat_t lat[3];

#define CC_HANDLER(i)                                                       \
RAM_FN static void cc##i##_isr(void *arg)                                  \
{                                                                          \
    uint32_t now = ccount();                                               \
    lat_t *l = arg;                                                        \
    uint32_t d = now - l->target;                                          \
    __asm__ volatile("wsr.ccompare" #i " %0" :: "r"(l->target - 1));      \
    if (d < l->min) l->min = d;                                            \
    if (d > l->max) l->max = d;                                            \
    l->sum += d; l->n++; l->done = 1;                                      \
}
CC_HANDLER(0)
CC_HANDLER(1)
CC_HANDLER(2)

#define ARM(i, l) do {                                                      \
    (l)->done = 0;                                                         \
    (l)->target = ccount() + 2000;                                         \
    __asm__ volatile("wsr.ccompare" #i " %0; rsync" :: "r"((l)->target)); \
} while (0)

int main(void)
{
    static const int cpu_int[3] = { 6, 15, 16 }, level[3] = { 1, 3, 5 };
    irq_fn_t fn[3] = { cc0_isr, cc1_isr, cc2_isr };

    printf("\nesp-ndf interrupt latency (%d samples per level, cycles @ 240 MHz)\n", N);
    for (int k = 0; k < 3; k++) {
        lat_t *l = &lat[k];
        l->min = ~0u;
        uint32_t far = ccount() - 1;     /* clear stale match before enabling */
        if (k == 0) __asm__ volatile("wsr.ccompare0 %0" :: "r"(far));
        if (k == 1) __asm__ volatile("wsr.ccompare1 %0" :: "r"(far));
        if (k == 2) __asm__ volatile("wsr.ccompare2 %0" :: "r"(far));
        irq_install(cpu_int[k], fn[k], l);
        irq_enable_all();
        for (int i = 0; i < N; i++) {
            if (k == 0) ARM(0, l); else if (k == 1) ARM(1, l); else ARM(2, l);
            while (!l->done) {}
        }
        printf("level %d (cpu int %2d): min %3lu  avg %3lu  max %3lu cycles  = %lu / %lu ns\n",
               level[k], cpu_int[k], l->min, (uint32_t)(l->sum / l->n), l->max,
               l->min * 25 / 6, l->max * 25 / 6);
    }
    return 0;
}
