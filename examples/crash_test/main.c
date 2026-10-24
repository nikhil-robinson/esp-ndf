/* Crash record: the first boot dies in a function on purpose (illegal instruction), restarts (NDF_PANIC_RESTART), and
 * the second boot reads the record from RTC memory: reason, PC inside that function, backtrace, count. */
#include "console.h"
#include "system.h"
#include "sys.h"

__attribute__((noinline)) void crash_here(void) { __builtin_trap(); }

int main(void)
{
    sys_crash_t c;
    if (sys_last_crash(&c)) {
        bool in_fn = c.pc >= (uint32_t)crash_here && c.pc < (uint32_t)crash_here + 16;
        printf("crash_test: last crash \"%s\" core %d pc 0x%08lx (%s crash_here) after %lu ms, %d backtrace pcs, count %lu\n",
               c.why, c.core, c.pc, in_fn ? "in" : "NOT in", c.uptime_ms, c.n_bt, sys_crash_count());
        printf("crash_test %s done\n", in_fn && c.n_bt >= 2 && sys_crash_count() >= 1 && !sys_last_crash(0) ? "PASS" : "FAIL");
        for (;;) {}
    }
    printf("crash_test: crashing on purpose\n");
    delay_ms(20);
    crash_here();
}
