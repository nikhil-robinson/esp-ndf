/* Runaway recursion with the boot stack guard (NDF_STACK_GUARD): expect "*** PANIC ... stack overflow" with the
 * backtrace, before the stack runs into .bss (a canary just below the stack must stay intact). */
#include "console.h"
#include "sys.h"

static volatile uint32_t canary = 0xC0FFEE11;                       /* .bss/.data: below _stack0_bot */
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static __attribute__((noipa)) int down(int n) { volatile uint8_t pad[128]; pad[0] = n; return down(n + 1) + pad[0]; }

int main(void)
{
    delay_ms(200);
    printf("stack_overflow: canary %08lx, recursing\n", (unsigned long)canary);
    down(0);
    printf("not reached\n");
    for (;;) {}
}
