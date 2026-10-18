/* Naked level-5 interrupt vs C handler: entry, exit, time lost by the loop. */
#include "sys.h"
#include "irq.h"
#include "console.h"
#include "xtensa.h"

volatile uint32_t naked_probe[4];               /* saved a2, t_in, done, t_out */
extern void naked_cc2(void);

int main(void)
{
    printf("\n=== naked level-5 interrupt (cycles, 240 = 1 us) ===\n");
    uint32_t far = ccount() - 1;
    __asm__ volatile("wsr.ccompare2 %0" :: "r"(far));
    irq_naked_install(16, naked_cc2);
    irq_enable_all();
    uint32_t emin = ~0u, emax = 0, xmin = ~0u, xmax = 0, smin = ~0u, smax = 0;
    for (int i = 0; i < 20000; i++) {
        naked_probe[2] = 0;
        uint32_t target = ccount() + 2000;
        __asm__ volatile("wsr.ccompare2 %0; rsync" :: "r"(target));
        uint32_t prev = ccount(), now;
        for (;;) { now = ccount(); if (naked_probe[2]) break; prev = now; }
        uint32_t e = naked_probe[1] - target, x = now - naked_probe[3], s = now - prev;
        if (e < emin) emin = e;
        if (e > emax) emax = e;
        if (x < xmin) xmin = x;
        if (x > xmax) xmax = x;
        if (s < smin) smin = s;
        if (s > smax) smax = s;
    }
    printf("  naked level 5: entry %lu..%lu  exit %lu..%lu  loop lost %lu..%lu\n", emin, emax, xmin, xmax, smin, smax);
    printf("  (C handler at level 5 measured: entry 47, exit 45, loop lost 109)\n=== done ===\n");
    for (;;) ;
}
