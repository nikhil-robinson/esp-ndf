/* Interrupt allocation on the ESP32-P4 CLIC. Each core has 32 external lines (CLIC ids 16..47) whose level is
 * programmable, so unlike the S3 there are no fixed-level lines: any free line takes any level 1-7. Each attached line
 * is CLIC-vectored: mtvt[16 + n] = irq_entry (vectors.S) or the naked handler itself.
 * CLIC per-interrupt word at 0x20801000 + 4 * id (this core's view): byte 0 IP, 1 IE, 2 ATTR (b0 SHV, b2:1 trigger,
 * b7:6 mode), 3 CTL (level << 5); read from the ROM's esprv_intc_int_enable / _set_type / _set_priority (P4 ROM). */
#include "irq.h"
#include "cpu.h"
#include "soc.h"

#define CLIC_CTRL(id)  (0x20801000u + 4u * (id))
#define CLIC_ID(n)     (16 + (n))
#define ATTR_SHV_LEVEL_M 0xC1u           /* machine mode, level-triggered, vectored */
#define ATTR_SHV_EDGE_M  0xC3u           /* machine mode, rising edge (software sets IP), vectored */

typedef struct { irq_fn_t fn; void *arg; } irq_slot_t;
irq_slot_t irq_tab[SOC_CORES][32];        /* used by vectors.S: layout fixed */
static int16_t line_src[SOC_CORES][32];   /* source + 1 routed to the line, 0 = free */

extern void irq_entry(void);
#define MTVT_INIT { [0 ... 47] = irq_entry }
/* CLIC vector tables, one per core, 48 entries; alignment: the spec asks for the next power of two >= 4 * entries */
void (*irq_mtvt0[48])(void) __attribute__((aligned(256))) = MTVT_INIT;
void (*irq_mtvt1[48])(void) __attribute__((aligned(256))) = MTVT_INIT;

static void (**mtvt(void))(void) { return core_id() ? irq_mtvt1 : irq_mtvt0; }

static void line_setup_attr(int n, int level, void (*entry)(void), uint8_t attr)
{
    volatile uint8_t *c = (volatile uint8_t *)CLIC_CTRL(CLIC_ID(n));
    c[1] = 0;                                         /* off while it changes */
    mtvt()[CLIC_ID(n)] = entry;
    cpu_barrier();
    c[2] = attr;
    c[3] = (uint8_t)CLIC_LVL_BYTE(level);
    c[0] = 0;
    c[1] = 1;
}
static void line_setup(int n, int level, void (*entry)(void)) { line_setup_attr(n, level, entry, ATTR_SHV_LEVEL_M); }

static void line_off(int n) { ((volatile uint8_t *)CLIC_CTRL(CLIC_ID(n)))[1] = 0; }

static void route(int core, int src, int id) { REG(INTMTX_MAP(core, src)) = (uint32_t)id; }

static int line_free(int core)
{
    for (int n = 0; n < 32; n++) if (!line_src[core][n] && !irq_tab[core][n].fn) return n;
    return -1;
}

/* ponytail: no line sharing (32 programmable lines per core cover every driver at once); share like the S3 if an
 * application ever routes more than 32 sources to one core */
static int attach(int src, int level, irq_fn_t fn, void (*entry)(void), void *arg)
{
    if (level < 1 || level > 7) return -1;
    uint32_t t = irq_lock();
    int core = core_id(), n = line_free(core);
    if (n >= 0) {
        irq_tab[core][n] = (irq_slot_t){ fn, arg };
        line_src[core][n] = (int16_t)(src + 1);
        line_setup(n, level, entry);
        route(core, src, CLIC_ID(n));
    }
    irq_unlock(t);
    return n;
}

static void stray(void *a) { (void)a; }              /* a naked line's slot: marks it used */

int irq_attach(int src, int level, irq_fn_t fn, void *arg)
{
    return level <= 5 ? attach(src, level, fn, irq_entry, arg) : -1;
}

int irq_attach_nmi(int src, irq_fn_t fn, void *arg)
{
    int core = core_id();
    for (int n = 0; n < 32; n++)
        if (line_src[core][n] == src + 1) { irq_detach(src, n); break; }
    return fn ? attach(src, 7, fn, irq_entry, arg) : 0;
}

void irq_detach(int src, int n)
{
    if (n < 0 || n > 31) return;
    uint32_t t = irq_lock();
    int core = core_id();
    route(core, src, 0);                              /* id 0: never enabled = disconnected */
    line_off(n);
    mtvt()[CLIC_ID(n)] = irq_entry;
    irq_tab[core][n] = (irq_slot_t){ 0, 0 };
    line_src[core][n] = 0;
    irq_unlock(t);
}

void irq_install(int n, irq_fn_t fn, void *arg)
{
    if (n < 0 || n > 31) return;
    uint32_t t = irq_lock();
    irq_tab[core_id()][n] = (irq_slot_t){ fn, arg };
    line_setup_attr(n, 1, irq_entry, ATTR_SHV_EDGE_M);   /* no matrix source: a software interrupt (CLIC IP byte) */
    irq_unlock(t);
}

/* Naked handler: the CLIC jumps straight into it (MIE off, level raised, mepc/mcause saved by hardware). It saves what
 * it uses, clears its source and ends with mret. No frame: the latency floor. */
int irq_naked(int src, int level, void (*handler)(void))
{
    return attach(src, level, stray, handler, 0);
}

void irq_naked_install(int n, void (*handler)(void))
{
    if (n < 0 || n > 31) return;
    uint32_t t = irq_lock();
    irq_tab[core_id()][n] = (irq_slot_t){ stray, 0 };
    line_setup(n, 5, handler);
    irq_unlock(t);
}
