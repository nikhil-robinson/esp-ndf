/* Crash handling over real restarts (NDF_PANIC_RESTART): step 0 dies on an illegal instruction, step 1 smashes a
 * stack canary (-fstack-protector-strong), step 2 masks interrupts for 3 s (interrupt watchdog). Each next boot reads
 * the crash record from RTC memory and checks reason, place and count. Later crashes wait 5.5 s after boot so the
 * crash-loop brake (3 crashes within 5 s of boot) does not stop the sequence. */
#include "console.h"
#include "system.h"
#include "sys.h"
#include "cpu.h"
#include <string.h>

#define STEP  (*(volatile uint32_t *)0x50001F70u)
#define MAGIC (*(volatile uint32_t *)0x50001F74u)
#define HOOK  (*(volatile uint32_t *)0x50001F78u)      /* ndf_on_panic runs: one bit per step */

void ndf_on_panic(void) { HOOK |= 1u << STEP; }

__attribute__((noinline)) void crash_here(void) { __builtin_trap(); }

__attribute__((noinline)) void smash(const char *src, size_t n)      /* writes past its buffer: the canary catches it */
{
    char buf[16];
    memcpy(buf, src, n);
    __asm__ volatile("" :: "r"(buf) : "memory");
}

int main(void)
{
    int rc0 = sys_reset_cause(0);                      /* any reset but our own panic restarts / watchdog: start over */
    if (MAGIC != 0x43525348u || !(rc0 == RESET_SW_SYS || rc0 == RESET_SW_CPU || rc0 == RESET_TG1WDT_SYS || rc0 == RESET_TG1WDT_CPU))
        { MAGIC = 0x43525348u; STEP = 0; HOOK = 0; }
    sys_crash_t c; bool had = sys_last_crash(&c);
    if (had) printf("crash_test: last crash \"%s\" pc 0x%08lx, count %lu\n", c.why, c.pc, sys_crash_count());
    switch (STEP) {
    case 0:
        printf("crash_test: step 0, illegal instruction\n"); delay_ms(20);
        STEP = 1; crash_here();
        break;
    case 1: {
        bool in_fn = had && c.pc >= (uint32_t)crash_here && c.pc < (uint32_t)crash_here + 16;
        printf("%s illegal instruction recorded in crash_here (%d backtrace pcs)\n", in_fn && c.n_bt >= 2 ? "PASS" : "FAIL", c.n_bt);
        delay_ms(5500);
        printf("crash_test: step 1, overflow a 16-byte stack buffer\n"); delay_ms(20);
        static char big[64]; memset(big, 0x41, sizeof big);
        STEP = 2; smash(big, sizeof big);
        printf("FAIL smash returned\n");
        break;
    }
    case 2:
        printf("%s canary recorded (\"%s\")\n", had && strstr(c.why, "canary") ? "PASS" : "FAIL", had ? c.why : "");
        delay_ms(5500);
        printf("crash_test: step 2, interrupts off for 3 s\n"); delay_ms(20);
        STEP = 3;
        { uint32_t ps = irq_lock(); uint32_t t = ccount(); while (ccount() - t < 720000000u) {} irq_unlock(ps); }
        printf("FAIL interrupt watchdog did not fire\n");
        break;
    case 3: {
        int rc = sys_reset_cause(0);
        bool wd = (had && strstr(c.why, "atchdog")) || rc == RESET_TG1WDT_SYS || rc == RESET_TG1WDT_CPU;
        printf("%s interrupt watchdog (\"%s\", reset cause %s)\n", wd ? "PASS" : "FAIL", had ? c.why : "", sys_reset_cause_str(rc));
        /* the masked-core watchdog is a hardware reset (pads back to reset state), no code runs: hook on steps 1-2 */
        printf("%s safe-state hook ran on both software faults (0x%lx)\n", (HOOK & 0x6) == 0x6 ? "PASS" : "FAIL", HOOK);
        MAGIC = 0;
        printf("crash_test done\n");
        break;
    }
    }
    for (;;) {}
}
