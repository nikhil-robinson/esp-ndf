/* Round-robin cooperative scheduler. Task 0..N-1 run on their own stacks; the scheduler context is the caller of
 * coop_run(). A new task starts in coop_entry (its context's a0 / a12 / a13 hold entry, fn, arg). */
#include "ndf_config.h"
#include "coop.h"
#include "sys.h"
#include "debug.h"

#include "cpu.h"
#ifndef COOP_CTX_WORDS
#define COOP_CTX_WORDS 6          /* coop_switch context (Xtensa CALL0: a0 a1 a12-a15); arch cpu.h may set it */
#endif

#define MAXT 8
typedef struct { uint32_t ctx[COOP_CTX_WORDS]; void (*fn)(void *); void *arg; uint32_t wake_ms, lo, hi; uint8_t state; } task_t;   /* 0 free 1 ready 2 done */
static task_t t[MAXT];
static uint32_t sched_ctx[COOP_CTX_WORDS], s_lo, s_hi;
static bool guard;                                   /* hardware stack guard on: move its window on every switch */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
static int cur = -1;

extern void coop_switch(uint32_t *save, const uint32_t *load);

static void coop_entry(void)
{
    if (guard) stack_guard_bounds(t[cur].lo, t[cur].hi);
    t[cur].fn(t[cur].arg);
    t[cur].state = 2;
    if (guard) stack_guard_bounds(MIN(s_lo, t[cur].lo), MAX(s_hi, t[cur].hi));
    coop_switch(t[cur].ctx, sched_ctx);                      /* never resumed */
    for (;;) {}
}

int coop_spawn(void (*fn)(void *), void *arg, void *stack, size_t n)
{
    for (int i = 0; i < MAXT; i++) if (!t[i].state) {
        uint32_t top = ((uint32_t)stack + n) & ~15u;
        uint32_t lo = (uint32_t)stack + (n >= 4 * NDF_STACK_GUARD ? NDF_STACK_GUARD : 0);   /* margin as the boot stacks: */
        t[i] = (task_t){ .fn = fn, .arg = arg, .state = 1, .lo = lo, .hi = top };           /* frames still fit below */
        t[i].ctx[0] = (uint32_t)coop_entry;                  /* a0: "return" into the entry */
        t[i].ctx[1] = top - 16;                              /* a1: stack (16 B for the callee's spill area) */
        return i;
    }
    return -1;
}

int coop_self(void) { return cur; }

void coop_yield(void)
{
    if (cur < 0) return;
    int me = cur;
    if (guard) stack_guard_bounds(MIN(s_lo, t[me].lo), MAX(s_hi, t[me].hi));
    coop_switch(t[me].ctx, sched_ctx);
    if (guard) stack_guard_bounds(t[me].lo, t[me].hi);     /* back on this task's stack */
}

void coop_sleep_ms(uint32_t ms)
{
    if (cur < 0) { delay_ms(ms); return; }
    t[cur].wake_ms = (time_ms() + ms) | 1;                  /* never 0 (= awake); 1 ms of slack at worst */
    coop_yield();
}

void coop_run(void)
{
    stack_guard_get(&s_lo, &s_hi);
    guard = stack_guard_bounds(s_lo, s_hi);
    for (;;) {
        int live = 0;
        for (int i = 0; i < MAXT; i++) {
            if (t[i].state != 1) continue;
            live++;
            if (t[i].wake_ms) {                                     /* sleeping: the only case that reads the timer */
                if ((int32_t)(time_ms() - t[i].wake_ms) < 0) continue;
                t[i].wake_ms = 0;
            }
            cur = i;
            if (guard) stack_guard_bounds(MIN(s_lo, t[i].lo), MAX(s_hi, t[i].hi));
            coop_switch(sched_ctx, t[i].ctx);
            if (guard) stack_guard_bounds(s_lo, s_hi);
            cur = -1;
            if (t[i].state == 2) t[i].state = 0;
        }
        if (!live) return;
    }
}
