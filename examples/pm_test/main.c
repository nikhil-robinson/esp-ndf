/* DFS: pm_config(240, 80); lock -> 240 MHz, unlock -> 80 MHz (CPU clock measured against the system timer); lock and
 * unlock cost (cycles at the start clock); console and timing still right at 80 MHz. Sibling: bench/idf_pm. */
#include "console.h"
#include "pm.h"
#include "clk.h"
#include "sys.h"
#include "xtensa.h"

static uint32_t measured_mhz(void)
{
    uint64_t t0 = time_us(); uint32_t c0 = ccount();
    while (time_us() - t0 < 2000) {}
    return (ccount() - c0) / 2000;
}

int main(void)
{
    delay_ms(200);
    int r = pm_config(240, 80);
    uint32_t idle = measured_mhz();
    uint32_t t = ccount(); pm_lock(); uint32_t tl = ccount() - t;        /* counted at 80 MHz, then ends at 240 */
    uint32_t busy = measured_mhz();
    pm_lock(); pm_unlock();                                            /* nested: stays at max */
    uint32_t nested = measured_mhz();
    t = ccount(); pm_unlock(); uint32_t tu = ccount() - t;
    uint32_t back = measured_mhz();
    uint64_t d0 = time_us(); delay_ms(10); uint32_t dms = (uint32_t)(time_us() - d0);   /* delays right at 80 MHz */
    uint32_t best_l = ~0u, best_u = ~0u;
    for (int i = 0; i < 100; i++) {
        uint64_t a = time_us(); pm_lock(); uint64_t b = time_us(); pm_unlock(); uint64_t c = time_us();
        (void)a; (void)b; (void)c;
        uint32_t c0 = ccount(); pm_lock(); uint32_t c1 = ccount(); pm_unlock(); uint32_t c2 = ccount();
        if (c1 - c0 < best_l) best_l = c1 - c0;
        if (c2 - c1 < best_u) best_u = c2 - c1;
    }
    int ok = !r && idle >= 78 && idle <= 82 && busy >= 238 && busy <= 242 && nested >= 238 && back >= 78 && back <= 82 && dms >= 9990 && dms <= 10100 && pm_locks() == 0;
    printf("%s DFS: idle %lu MHz, locked %lu MHz, nested %lu MHz, unlocked %lu MHz, delay_ms(10) = %lu us | lock %lu cycles, unlock %lu cycles\n",
           ok ? "PASS" : "FAIL", (unsigned long)idle, (unsigned long)busy, (unsigned long)nested, (unsigned long)back, (unsigned long)dms,
           (unsigned long)best_l, (unsigned long)best_u);
    (void)tl; (void)tu;
    pm_config(240, 240);
    printf("pm_test %s done\n", ok ? "PASS" : "FAIL");
    for (;;) {}
}
