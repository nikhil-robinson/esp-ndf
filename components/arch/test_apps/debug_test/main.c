/* Profiling + debug: perf counters (cycles vs CCOUNT, instructions and loads of a known loop, read cost), a write
 * watchpoint (callback with the store's PC, the store completes), the stack guard (deep recursion crosses a limit:
 * callback, unwind normally) and its cost on a call-heavy loop. Sibling: bench/idf_perf (perfmon read cost). */
#include "console.h"
#include "debug.h"
#include "sys.h"
#include "cpu.h"

static volatile uint32_t target, hit_n = 99, hit_pc, guard_hit, guard_pc;
static volatile uint32_t arr[1000];
static void on_watch(int n, uint32_t pc) { hit_n = n; hit_pc = pc; }
static void on_guard(int core, uint32_t pc) { (void)core; guard_hit = 1; guard_pc = pc; }

static __attribute__((noinline)) void poke(void) { target = 0x1234; }
static __attribute__((noinline)) int deep(int n)
{
    volatile uint8_t pad[64]; pad[0] = n;
    if (guard_hit || n == 0) return pad[0];
    return deep(n - 1) + pad[0];
}
static __attribute__((noipa)) uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main(void)
{
    delay_ms(200);
    int fail = 0;
    /* perf counters */
    perf_setup(0, PERF_CYCLES); perf_setup(1, PERF_INSN); perf_start();
    uint32_t c0 = ccount(), p0 = perf_read(0), i0 = perf_read(1);
    __asm__ volatile("movi a8, 1000\n loop a8, 1f\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n 1:" ::: "a8");
    uint32_t c1 = ccount(), p1 = perf_read(0), i1 = perf_read(1);
    uint32_t dcyc = p1 - p0, dcc = c1 - c0, dins = i1 - i0;
    perf_setup(1, PERF_D_LOAD);
    uint32_t l0 = perf_read(1), s = 0; for (int i = 0; i < 1000; i++) s += arr[i]; uint32_t dld = perf_read(1) - l0;
    uint32_t r0 = ccount(); (void)perf_read(0); uint32_t rd = ccount() - r0;
    int okp = dcyc + 8 >= dcc && dcyc <= dcc + 8 && dins >= 10000 && dins < 10100 && dld >= 1000 && dld < 1100;
    printf("%s perf: 1000 x 10 nops: cycles %lu (CCOUNT %lu), instructions %lu; 1000 loads: %lu; read %lu cycles (incl. ccount)\n",
           okp ? "PASS" : "FAIL", (unsigned long)dcyc, (unsigned long)dcc, (unsigned long)dins, (unsigned long)dld, (unsigned long)rd);
    fail |= !okp;
    perf_stop();
    /* watchpoint */
    watch_set(1, &target, 4, WATCH_WRITE, on_watch);
    poke();
    uint32_t again = hit_n; target = 7;                              /* one-shot: second store not caught */
    int okw = hit_n == 1 && again == 1 && hit_pc >= (uint32_t)poke && hit_pc < (uint32_t)poke + 32 && target == 7;
    printf("%s watchpoint: hit n %lu at pc 0x%08lx (poke at %p), store completed, one-shot %d\n", okw ? "PASS" : "FAIL",
           (unsigned long)hit_n, (unsigned long)hit_pc, (void *)poke, again == 1);
    fail |= !okw;
    /* stack guard: limit 3 KB below here, recursion of ~100 B frames */
    extern char _stack0_bot[], _stack0_top[];
    uint32_t sp; __asm__ volatile("mov %0, a1" : "=r"(sp));
    stack_guard(sp - 3072, (uint32_t)_stack0_top, on_guard);
    deep(200);
    int okg = guard_hit && guard_pc >= (uint32_t)deep && guard_pc < (uint32_t)deep + 128;
    printf("%s stack guard: crossed SP %08lx at pc 0x%08lx (deep at %p)\n", okg ? "PASS" : "FAIL", (unsigned long)(sp - 3072),
           (unsigned long)guard_pc, (void *)deep);
    fail |= !okg;
    /* cost: fib(24) with the boot guard on vs off */
    stack_guard((uint32_t)_stack0_bot + NDF_STACK_GUARD, (uint32_t)_stack0_top, 0);
    uint32_t t = ccount(); volatile uint32_t f = fib(24); uint32_t ton = ccount() - t;
    stack_guard_off();
    t = ccount(); f = fib(24); uint32_t toff = ccount() - t; (void)f;
    stack_guard((uint32_t)_stack0_bot + NDF_STACK_GUARD, (uint32_t)_stack0_top, 0);
    printf("PASS stack guard cost: fib(24) %lu cycles guarded, %lu unguarded\n", (unsigned long)ton, (unsigned long)toff);
    printf("debug_test %s done\n", fail ? "FAIL" : "PASS");
    for (;;) {}
}
