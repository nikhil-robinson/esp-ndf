/* Interrupt handler tables for Xtensa LX7; dispatch itself is in
 * vectors.S. Levels 1-3 share a table searched per interrupt; levels 4-5
 * have one direct "fast" handler per level per core. */
#include "irq.h"
#include "xtensa.h"
#include "soc_intr.h"

typedef struct { irq_fn_t fn; void *arg; } irq_slot_t;
irq_slot_t irq_tab[SOC_CORES][32];        /* used by vectors.S: layout fixed */
irq_slot_t irq_fast[SOC_CORES][2];        /* levels 4, 5 */

/* Install into the right table; false if a fast level is already taken. */
static bool slot_set(int core, int n, irq_fn_t fn, void *arg)
{
    int lvl = soc_intr_level(n);
    if (lvl >= 4 && lvl <= 5) {
        irq_slot_t *f = &irq_fast[core][lvl - 4];
        if (fn && f->fn) return false;
        *f = (irq_slot_t){ fn, arg };
    }
    irq_tab[core][n] = (irq_slot_t){ fn, arg };   /* also marks n as used */
    return true;
}

static void intenable_set(int n, bool on)
{
    uint32_t ps = irq_lock(), e;
    __asm__ volatile("rsr.intenable %0" : "=r"(e));
    e = on ? e | BIT(n) : e & ~BIT(n);
    __asm__ volatile("wsr.intenable %0; rsync" :: "r"(e));
    irq_unlock(ps);
}

void irq_install(int n, irq_fn_t fn, void *arg)
{
    uint32_t ps = irq_lock();
    bool ok = slot_set(core_id(), n, fn, arg);
    irq_unlock(ps);
    if (ok) intenable_set(n, true);
}

int irq_attach(int src, int level, irq_fn_t fn, void *arg)
{
    int core = core_id();
    if (level < 1 || level > 5 || !fn) return -1;
    uint32_t ps = irq_lock();
    int n = -1;
    for (uint32_t free = soc_intr_level_mask[level] & SOC_INTR_PERIPH_LEVEL_MASK; free; free &= free - 1) {
        int c = __builtin_ctz(free);
        if (!irq_tab[core][c].fn) { n = c; break; }
    }
    if (n >= 0 && slot_set(core, n, fn, arg)) soc_intr_route(core, src, n);
    else n = -1;
    irq_unlock(ps);
    if (n >= 0) intenable_set(n, true);
    return n;
}

void irq_detach(int src, int n)
{
    intenable_set(n, false);
    soc_intr_route(core_id(), src, SOC_INTR_DISABLED);
    slot_set(core_id(), n, 0, 0);
}
