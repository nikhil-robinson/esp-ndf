#include "debug.h"
#include "console.h"
#include "sys.h"

static volatile uint32_t a, b[4] __attribute__((aligned(16)));
static volatile int hits, last_n;
static void on_hit(int n, uint32_t pc) { (void)pc; hits++; last_n = n; }

int main(void)
{
    int err = 0;
    if (watch_set(0, &a, 4, WATCH_WRITE, on_hit)) { printf("watch_set failed (debugger attached?)\n"); return 1; }
    a = 1;                                                     /* hit: handler runs, trigger cleared, the store completes */
    err += hits != 1 || last_n != 0 || a != 1;
    a = 2;                                                     /* no second hit: one-shot */
    err += hits != 1 || a != 2;
    watch_set(1, &a, 4, WATCH_READ, on_hit);
    uint32_t v = a;
    err += hits != 2 || last_n != 1 || v != 2;
    if (watch_set(2, b, 16, WATCH_READ | WATCH_WRITE, on_hit) == 0) {     /* NAPOT range of 16 bytes */
        b[3] = 7;
        err += hits != 3 || last_n != 2 || b[3] != 7;
    } else printf("only %d triggers implemented (third watch refused)\n", 2);
    perf_setup(0, PERF_CYCLES); perf_setup(1, PERF_INSN); perf_reset(0); perf_reset(1);
    for (volatile int i = 0; i < 1000; i++) {}
    err += perf_read(0) < perf_read(1) || perf_read(1) < 1000;
    printf("watch_test %s (hits %d, errors %d)\n", err ? "FAILED" : "done", hits, err);
    return err != 0;
}
