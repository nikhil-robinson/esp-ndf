/* Default panic policy (NDF_PANIC_RESTART 0) halts: after a fault the interrupt watchdog must not reset the chip.
 * Prints "panic_halt_test: fault" once per boot; tools check that it appears exactly once in 8 s (tools/run_app.py). */
#include "console.h"
#include "sys.h"

__attribute__((noinline)) static void crash_here(void) { __builtin_trap(); }

int main(void)
{
    delay_ms(200);
    printf("panic_halt_test: fault\n"); delay_ms(20);
    crash_here();
    for (;;) {}
}
