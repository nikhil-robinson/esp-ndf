/* Interrupt handler tables for Xtensa LX7; dispatch itself is in
 * vectors.S. Levels 1-3 share a table searched per interrupt; levels 4-5
 * have one direct "fast" handler per level per core. */
#include "irq.h"
#include "xtensa.h"
#include "soc_intr.h"
#include "memprot.h"

typedef struct { irq_fn_t fn; void *arg; } irq_slot_t;
irq_slot_t irq_tab[SOC_CORES][32];        /* used by vectors.S: layout fixed */
irq_slot_t irq_fast[SOC_CORES][2];        /* levels 4, 5 */

/* Line sharing (levels 1-3) once every line of a level is taken: the line's
 * slot runs shared_dispatch, which calls each member; every driver ISR checks
 * its own status first. Lines are only shared when they run out, so a lone
 * source keeps its direct dispatch and latency. */
typedef struct share { irq_fn_t fn; void *arg; int src; struct share *next; } share_t;
static share_t share_pool[NDF_IRQ_SHARES];           /* members of shared lines, both cores */
static share_t *shared[SOC_CORES][32];
static uint8_t line_src[SOC_CORES][32];    /* source of a line's single owner */

RAM_FN static void shared_dispatch(void *arg)
{
    for (share_t *s = *(share_t **)arg; s; s = s->next) s->fn(s->arg);
}

static share_t *share_new(irq_fn_t fn, void *arg, int src)
{
    for (int i = 0; i < (int)(sizeof share_pool / sizeof share_pool[0]); i++)
        if (!share_pool[i].fn) { share_pool[i] = (share_t){ fn, arg, src, 0 }; return &share_pool[i]; }
    return 0;
}

/* Install into the right table; false if a fast level is already taken. */
static bool slot_set(int core, int n, irq_fn_t fn, void *arg)
{
    int lvl = soc_intr_level(n);
    if (lvl >= 4 && lvl <= 5) {
        irq_slot_t *f = &irq_fast[core][lvl - 4];
        if (fn && f->fn && !irq_tab[core][n].fn) return false;   /* taken by another line; same line = replace */
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

/* no free line at `level`: join the line with the fewest members */
static int share_line(int core, int level, int src, irq_fn_t fn, void *arg)
{
    int best = -1, best_n = 99;
    for (uint32_t m = soc_intr_level_mask[level] & SOC_INTR_PERIPH_LEVEL_MASK; m; m &= m - 1) {
        int c = __builtin_ctz(m), k = 1;
        for (share_t *s = shared[core][c]; s; s = s->next) k++;
        if (shared[core][c]) k--;                          /* a shared line counts its list */
        if (k < best_n) { best_n = k; best = c; }
    }
    if (best < 0) return -1;
    share_t *me = share_new(fn, arg, src);
    if (!me) return -1;
    if (!shared[core][best]) {                            /* convert the single owner into a member */
        irq_slot_t *t = &irq_tab[core][best];
        share_t *first = share_new(t->fn, t->arg, line_src[core][best]);
        if (!first) { me->fn = 0; return -1; }
        shared[core][best] = first;
        *t = (irq_slot_t){ shared_dispatch, &shared[core][best] };
    }
    me->next = shared[core][best];
    shared[core][best] = me;
    soc_intr_route(core, src, best);
    return best;
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
    if (n >= 0 && slot_set(core, n, fn, arg)) { soc_intr_route(core, src, n); line_src[core][n] = src; }
    else if (n < 0 && level <= 3) n = share_line(core, level, src, fn, arg);
    else n = -1;
    irq_unlock(ps);
    if (n >= 0) intenable_set(n, true);
    return n;
}

void irq_detach(int src, int n)
{
    int core = core_id();
    soc_intr_route(core, src, SOC_INTR_DISABLED);
    if (shared[core][n]) {                                /* drop only this member */
        uint32_t ps = irq_lock();
        for (share_t **p = &shared[core][n]; *p; p = &(*p)->next)
            if ((*p)->src == src) { (*p)->fn = 0; *p = (*p)->next; break; }
        bool empty = !shared[core][n];
        irq_unlock(ps);
        if (!empty) return;
    }
    intenable_set(n, false);
    slot_set(core, n, 0, 0);
}

/* ---- naked level 4/5 handlers ---------------------------------------------
 * Patch the vector's jump (a 3-byte J instruction, written through the IRAM
 * data alias) to the naked redirect, which jumps to irq_naked_fn[level][core].
 * The C dispatch path is untouched while no naked handler is installed. */
uint32_t irq_naked_fn[2][SOC_CORES];           /* used by vectors.S: [level-4][core] */
extern char _vec_l4_jump0[], _vec_l5_jump0[], _vec_l4_jump1[], _vec_l5_jump1[];
extern char _l4_naked_0[], _l4_naked_1[], _l5_naked_0[], _l5_naked_1[];

static void patch_j(char *at, char *to)
{
    int32_t off = (int32_t)(to - at - 4);               /* J: target = PC + 4 + offset (18-bit) */
    uint32_t insn = 0x06 | ((uint32_t)off & 0x3FFFF) << 6;
    volatile uint8_t *d = (volatile uint8_t *)((uint32_t)at - 0x6F0000);   /* DRAM alias of IRAM */
    uint32_t ps = irq_lock();
    bool opened = memprot_code_write(true);             /* code is store-protected (memprot) */
    d[0] = insn; d[1] = insn >> 8; d[2] = insn >> 16;
    __asm__ volatile("memw; isync");
    if (opened) memprot_code_write(false);
    irq_unlock(ps);
}

/* point this core's level-4/5 vector at handler: direct J if in range, else via the redirect */
static void naked_vector(int core, int level, void (*handler)(void))
{
    char *at = level == 4 ? (core ? _vec_l4_jump1 : _vec_l4_jump0) : (core ? _vec_l5_jump1 : _vec_l5_jump0);
    int32_t off = (int32_t)((char *)handler - at - 4);
    irq_naked_fn[level - 4][core] = (uint32_t)handler;
    if (off >= -131072 && off < 131072) patch_j(at, (char *)handler);
    else patch_j(at, level == 4 ? (core ? _l4_naked_1 : _l4_naked_0) : (core ? _l5_naked_1 : _l5_naked_0));
}

int irq_naked(int src, int level, void (*handler)(void))
{
    if (level < 4 || level > 5) return -1;
    int core = core_id(), n = -1;
    uint32_t ps = irq_lock();
    for (uint32_t m = soc_intr_level_mask[level] & SOC_INTR_PERIPH_LEVEL_MASK; m; m &= m - 1) {
        int c = __builtin_ctz(m);
        if (!irq_tab[core][c].fn) { n = c; break; }
    }
    if (n < 0) { irq_unlock(ps); return -1; }
    irq_tab[core][n] = (irq_slot_t){ (irq_fn_t)handler, 0 };   /* marks the line used */
    if (src >= 0) soc_intr_route(core, src, n);
    irq_unlock(ps);
    naked_vector(core, level, handler);
    intenable_set(n, true);
    return n;
}

/* naked handler on an internal CPU interrupt (CCOMPARE, software) */
void irq_naked_install(int n, void (*handler)(void))
{
    int lvl = soc_intr_level(n), core = core_id();
    if (lvl < 4 || lvl > 5) return;
    irq_tab[core][n] = (irq_slot_t){ (irq_fn_t)handler, 0 };
    naked_vector(core, lvl, handler);
    intenable_set(n, true);
}

/* NMI: CPU interrupt 14 (level 7, edge from the matrix), one source + handler per core. Runs even inside
 * irq_lock(): for last-chance reports (watchdog with interrupts masked). The handler must not use the FPU/PIE. */
irq_slot_t irq_nmi[SOC_CORES];                 /* used by vectors.S */

int irq_attach_nmi(int src, irq_fn_t fn, void *arg)
{
    int core = core_id();
    if (!fn) { soc_intr_route(core, src, SOC_INTR_DISABLED); intenable_set(14, false); irq_nmi[core].fn = 0; return 0; }
    irq_nmi[core] = (irq_slot_t){ fn, arg };
    soc_intr_route(core, src, 14);
    intenable_set(14, true);
    return 14;
}
