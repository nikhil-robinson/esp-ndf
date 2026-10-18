/* SYSTIMER unit 0: 64-bit time base + the software-timer comparator (target 0). */
#include "sys.h"
#include "swtimer.h"
#include "soc.h"
#include "irq.h"
#include "xcore.h"
#include "xtensa.h"

uint64_t time_ticks(void)
{
    uint32_t hi, lo;
    do {                                  /* the other core may latch a new value between the reads */
        REG(SYSTIMER_UNIT0_OP) = BIT(30);
        while (!(REG(SYSTIMER_UNIT0_OP) & BIT(29))) {}
        hi = REG(SYSTIMER_UNIT0_HI);
        lo = REG(SYSTIMER_UNIT0_LO);
    } while (hi != REG(SYSTIMER_UNIT0_HI));
    return (uint64_t)hi << 32 | lo;
}

uint64_t time_us(void) { return time_ticks() >> 4; }

static swtimer_t *head;                   /* sorted by deadline */
static xc_spin_t lock;
static bool irq_on;

static void arm(void)                     /* lock held; program target 0 for head */
{
    if (!head) { REG(SYSTIMER_INT_ENA) &= ~BIT(0); return; }
    uint64_t at = head->when;
    for (uint32_t d = 2;; d <<= 1) {      /* never set a target that has already passed */
        uint64_t now = time_ticks();
        if (at < now + d) at = now + d;
        REG(SYSTIMER_TARGET0_HI) = at >> 32;
        REG(SYSTIMER_TARGET0_LO) = (uint32_t)at;
        REG(SYSTIMER_COMP0_LOAD) = 1;
        if (time_ticks() < at) break;
        at = 0;
    }
    REG(SYSTIMER_INT_ENA) |= BIT(0);
}

static void unlink(swtimer_t *t)
{
    for (swtimer_t **p = &head; *p; p = &(*p)->next)
        if (*p == t) { *p = t->next; break; }
    t->active = false;
}

static void insert(swtimer_t *t)
{
    swtimer_t **p = &head;
    while (*p && (*p)->when <= t->when) p = &(*p)->next;
    t->next = *p; *p = t; t->active = true;
}

static void swt_isr(void *arg)
{
    (void)arg;
    REG(SYSTIMER_INT_CLR) = BIT(0);
    (void)REG(SYSTIMER_INT_ST);           /* posted write: make sure the clear landed */
    uint32_t ps = irq_lock(); xc_spin_lock(&lock);
    uint64_t now = time_ticks();
    while (head && (head->when <= now || head->when <= (now = time_ticks()))) {
        swtimer_t *t = head;
        head = t->next; t->active = false;
        if (t->period) { t->when += t->period; insert(t); }
        void (*fn)(void *) = t->fn; void *a = t->arg;
        xc_spin_unlock(&lock); irq_unlock(ps);
        fn(a);                            /* may start/stop timers */
        ps = irq_lock(); xc_spin_lock(&lock);
    }
    arm();
    xc_spin_unlock(&lock); irq_unlock(ps);
}

void swt_start_ticks(swtimer_t *t, uint64_t first_at, uint32_t period, void (*fn)(void *), void *arg)
{
    uint32_t ps = irq_lock(); xc_spin_lock(&lock);
    if (!irq_on) {
        REG(SYSTIMER_TARGET0_CONF) = 0;   /* unit 0, one-shot */
        REG(SYSTIMER_CONF) |= BIT(24);
        irq_on = irq_attach(SRC_SYSTIMER_TARGET0, NDF_SWTIMER_LEVEL, swt_isr, 0) >= 0;
    }
    if (t->active) unlink(t);
    t->when = first_at; t->period = period; t->fn = fn; t->arg = arg;
    insert(t);
    if (head == t) arm();
    xc_spin_unlock(&lock); irq_unlock(ps);
}

void swt_start(swtimer_t *t, uint32_t us, bool periodic, void (*fn)(void *), void *arg)
{
    uint64_t ticks = (uint64_t)us * 16;
    swt_start_ticks(t, time_ticks() + ticks, periodic ? (uint32_t)ticks : 0, fn, arg);
}

void swt_stop(swtimer_t *t)
{
    uint32_t ps = irq_lock(); xc_spin_lock(&lock);
    if (t->active) { bool was_head = head == t; unlink(t); if (was_head) arm(); }
    xc_spin_unlock(&lock); irq_unlock(ps);
}
