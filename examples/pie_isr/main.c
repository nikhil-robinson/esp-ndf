/* PIE vector unit inside interrupt handlers (lazy save, arch/xtensa/vectors.S, cause 35). Core 0's main loop runs
 * dsp_dot_s16 (state in q registers + ACCX across its loop) on 8 KB; core 1 storms a level-1 handler and a level-3/5
 * handler that also run dsp_dot_s16 (and floats). Checks: every result equals its reference. Round-trip cost from
 * core 1: PIE handler vs integer handler. */
#include "irq.h"
#include "dsp.h"
#include "sys.h"
#include "console.h"
#include "xtensa.h"

#define FROM_CPU(n) (0x600C0030u + 4 * (n))
static int16_t a[4096] __attribute__((aligned(16))), b[4096] __attribute__((aligned(16)));
static int16_t ha[64] __attribute__((aligned(16))), hb[64] __attribute__((aligned(16)));
static int32_t ref_main, ref_h;
static volatile uint32_t n1, n3, bad1, bad3, go, stop, done1, rt_pie, rt_int, use_pie, pong;
static volatile float fin = 2.25f;

static void h1(void *x) { (void)x; REG(FROM_CPU(0)) = 0; (void)REG(FROM_CPU(0)); if (dsp_dot_s16(ha, hb, 64) != ref_h) bad1++; n1++; }
static void h3(void *x)
{
    (void)x; REG(FROM_CPU(2)) = 0; (void)REG(FROM_CPU(2));
    if (use_pie) { if (dsp_dot_s16(hb, ha, 64) != ref_h || fin * 2.0f != 4.5f) bad3++; }
    n3++; pong = n3;
}
static void core1(void)
{
    while (!go) ;
    while (!stop) { REG(FROM_CPU(0)) = 1; delay_us(3); REG(FROM_CPU(2)) = 1; delay_us(20); }
    for (int k = 0; k < 2; k++) {
        use_pie = k; uint32_t best = ~0u;
        for (int i = 0; i < 2000; i++) { uint32_t p = pong, t = ccount(); REG(FROM_CPU(2)) = 1; while (pong == p) ; t = ccount() - t; if (t < best) best = t; }
        if (k) rt_pie = best; else rt_int = best;
    }
    done1 = 1;
    for (;;) ;
}

int main(void)
{
    delay_ms(200);
    for (int i = 0; i < 4096; i++) { a[i] = (int16_t)(i * 37 - 2000); b[i] = (int16_t)(3000 - i * 11); }
    for (int i = 0; i < 64; i++) { ha[i] = (int16_t)(i * 101); hb[i] = (int16_t)(500 - i * 7); }
    ref_main = dsp_dot_s16(a, b, 4096); ref_h = dsp_dot_s16(ha, hb, 64);
    int fail = 0;
    cpu1_start(core1);
    for (int lvl = 3; lvl <= 5; lvl += 2) {
        n1 = n3 = bad1 = bad3 = 0; use_pie = 1; stop = 0; done1 = 0;
        int x = irq_attach(79, 1, h1, 0), y = irq_attach(81, lvl, h3, 0);
        go = 1;
        uint32_t badm = 0;
        for (int r = 0; r < 20000; r++) if (dsp_dot_s16(a, b, 4096) != ref_main) badm++;
        stop = 1; while (!done1) ;
        int pass = x >= 0 && y >= 0 && !badm && !bad1 && !bad3 && n1 > 1000 && n3 > 1000;
        printf("%s PIE in ISR, L1 + L%d: main wrong %lu of 20000, L1 runs %lu bad %lu, L%d runs %lu bad %lu | round trip int %lu, PIE+float %lu cycles\n",
               pass ? "PASS" : "FAIL", lvl, (unsigned long)badm, (unsigned long)n1, (unsigned long)bad1, lvl, (unsigned long)n3,
               (unsigned long)bad3, (unsigned long)rt_int, (unsigned long)rt_pie);
        fail |= !pass;
        irq_detach(79, x); irq_detach(81, y);
        go = 0; if (lvl == 3) cpu1_start(core1);
    }
    printf("pie_isr %s done\n", fail ? "FAIL" : "PASS");
    for (;;) ;
}
