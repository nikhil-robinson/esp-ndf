/* FPU inside interrupt handlers (lazy save, arch/xtensa/vectors.S). Core 0's main loop keeps float state in the FP
 * registers; core 1 raises CPU_INTR_FROM_CPU_0 (level 1 handler: long float loop) and FROM_CPU_2 (level 3 or 5:
 * short float work) so the second preempts the first. Checks: main result bit-equal to a run without interrupts,
 * every handler result right. Timing from core 1: round trip of a float handler vs an integer one. */
#include "irq.h"
#include "xcore.h"
#include "sys.h"
#include "console.h"
#include <math.h>

#define FROM_CPU(n) (0x600C0030u + 4 * (n))
static volatile uint32_t n1, n3, bad1, bad3, go, stop, done1, rt_fp, rt_int, use_fp, pong;
static volatile float in3 = 2.25f;


static void h1(void *a)                                         /* level 1: a few us of float work, preemptible */
{
    (void)a; REG(FROM_CPU(0)) = 0; (void)REG(FROM_CPU(0));
    float s = 0.0f;
    for (int i = 1; i <= 50; i++) s += (float)i * 0.5f;
    if (s != 637.5f) bad1++;
    n1++;
}
static void h3(void *a)                                         /* level 3/5: short float work */
{
    (void)a; REG(FROM_CPU(2)) = 0; (void)REG(FROM_CPU(2));
    if (use_fp) { float r = sqrtf(in3) * 3.0f + in3; if (r != 6.75f) bad3++; }
    n3++; pong = n3;
}

static float main_work(int n)                                   /* deterministic float chain */
{
    float x = 1.0f, y = 0.5f;
    for (int i = 0; i < n; i++) { x = x * 1.000013f + y; y = y * 0.999987f - x * 1e-7f; }
    return x + y;
}

static void core1(void)
{
    while (!go) ;
    while (!stop) {                                             /* storm: L1 then L3 shortly after (nests) */
        REG(FROM_CPU(0)) = 1;
        delay_us(5);
        REG(FROM_CPU(2)) = 1;
        delay_us(40);
    }
    for (int k = 0; k < 2; k++) {                               /* round trip: k = 0 int handler, 1 float handler */
        use_fp = k; uint32_t best = ~0u;
        for (int i = 0; i < 2000; i++) {
            uint32_t p = pong, t = ccount(); REG(FROM_CPU(2)) = 1; while (pong == p) ; t = ccount() - t;
            if (t < best) best = t;
        }
        if (k) rt_fp = best; else rt_int = best;
    }
    done1 = 1;
    for (;;) ;
}

int main(void)
{
    delay_ms(200);
    int fail = 0;
    float ref = main_work(200000);
    cpu1_start(core1);
    for (int lvl = 3; lvl <= 5; lvl += 2) {
        n1 = n3 = bad1 = bad3 = 0; use_fp = 1; stop = 0; done1 = 0;
        int a = irq_attach(79, 1, h1, 0), b = irq_attach(81, lvl, h3, 0);
        go = 1;
        float got = 0; int same = 1;
        for (int r = 0; r < 20; r++) { got = main_work(200000); same &= got == ref; }
        stop = 1;
        while (!done1) ;
        int pass = a >= 0 && b >= 0 && same && !bad1 && !bad3 && n1 > 100 && n3 > 100;
        printf("%s FPU in ISR, L1 + L%d: main bit-equal %d (%08lx), L1 runs %lu bad %lu, L%d runs %lu bad %lu | "
               "L%d round trip int handler %lu cycles, float handler %lu cycles\n", pass ? "PASS" : "FAIL", lvl, same,
               (unsigned long)(union { float f; uint32_t u; }){ got }.u, (unsigned long)n1, (unsigned long)bad1, lvl, (unsigned long)n3,
               (unsigned long)bad3, lvl, (unsigned long)rt_int, (unsigned long)rt_fp);
        fail |= !pass;
        irq_detach(79, a); irq_detach(81, b);
        go = 0; if (lvl == 3) { cpu1_start(core1); }
    }
    printf("fpu_isr %s done\n", fail ? "FAIL" : "PASS");
    for (;;) ;
}
