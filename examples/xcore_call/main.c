/* xc_call: core 0 runs a function on core 1 and gets its result (checks it ran on core 1), round trip in cycles;
 * xc_stall: core 1's counter stops while stalled and resumes after. Sibling: bench/idf_xcall (esp_ipc, esp_cpu_stall). */
#include "xcore.h"
#include "sys.h"
#include "console.h"

static volatile uint32_t ready, count;
static int where(void *a) { return core_id() * 1000 + (int)a; }

static void core1(void)
{
    ready = xc_call_init(3) == 0 ? 1 : 2;
    for (;;) count++;
}

int main(void)
{
    delay_ms(200);
    cpu1_start(core1);
    while (!ready) ;
    int r = -1, ok = ready == 1 && !xc_call(1, where, (void *)7, &r) && r == 1007;
    uint32_t mn = ~0u, sum = 0;
    for (int i = 0; i < 10000; i++) {
        uint32_t t = ccount(); xc_call(1, where, (void *)i, &r); t = ccount() - t;
        ok &= r == 1000 + i; if (t < mn) mn = t; sum += t;
    }
    xc_stall(1); delay_us(10); uint32_t c0 = count; delay_ms(2); uint32_t c1 = count;
    uint32_t t = ccount(); xc_unstall(1); while (count == c1) ; uint32_t tu = ccount() - t;
    delay_ms(1); uint32_t c2 = count;
    int ok2 = c1 == c0 && c2 > c1 + 1000;
    printf("%s xc_call: result %d, round trip min %lu avg %lu cycles | %s stall: frozen %d, resumed %d (%lu cycles to first count)\n",
           ok ? "PASS" : "FAIL", ok, (unsigned long)mn, (unsigned long)(sum / 10000), ok2 ? "PASS" : "FAIL", c1 == c0, c2 > c1 + 1000,
           (unsigned long)tu);
    printf("xcore_call %s done\n", ok && ok2 ? "PASS" : "FAIL");
    for (;;) ;
}
