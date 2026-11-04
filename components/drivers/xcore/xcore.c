/* Cross-core notification: SYSTEM_CPU_INTR_FROM_CPU_n (sources 79 + n) is a
 * level interrupt; the receiving core clears it in its handler. */
#include "xcore.h"
#include "spin.h"
#include "irq.h"

#define FROM_CPU_REG(n)  XC_FROM_CPU_REG(n)
#define SRC_FROM_CPU(n)  (79 + (n))              /* TRM interrupt table: CPU_INTR_FROM_CPU_0 = 79 */

typedef struct { int (*volatile fn)(void *); void *arg; volatile int ret; } xc_call_t;
static xc_call_t call_of[2];
static volatile uint8_t call_ready[2];
static xc_spin_t call_lock[2];
static void (*fn_of[2])(void *);
static void *arg_of[2];

RAM_FN static void notify_isr(void *a)
{
    int c = (int)a;
    if (!(REG(FROM_CPU_REG(c)) & 1)) return;       /* shared line: not a cross-core request */
    REG(FROM_CPU_REG(c)) = 0;                      /* level: clear before running the handler, */
    (void)REG(FROM_CPU_REG(c));                    /* and read back so the clear has landed (no double call) */
    xc_call_t *k = &call_of[c];
    if (k->fn) { k->ret = k->fn(k->arg); xc_barrier(); k->fn = 0; }   /* pending xc_call first */
    if (fn_of[c]) fn_of[c](arg_of[c]);
}

int xc_notify_init(void (*fn)(void *), void *arg, int level)
{
    int c = core_id();
    fn_of[c] = fn; arg_of[c] = arg;
    REG(FROM_CPU_REG(c)) = 0;
    return irq_attach(SRC_FROM_CPU(c), level, notify_isr, (void *)c) < 0 ? -1 : 0;
}

int xc_call_init(int level)
{
    int c = core_id();
    if (!fn_of[c] && xc_notify_init(0, 0, level)) return -1;
    call_ready[c] = 1;
    return 0;
}

int xc_call(int core, int (*fn)(void *), void *arg, int *ret)
{
    if (!(core >= 0 && core < 2)) return -1;      /* caller index: never past the hardware */
    int r;
    if (core == core_id()) r = fn(arg);
    else {
        if (!call_ready[core]) return -1;
        xc_spin_lock(&call_lock[core]);
        xc_call_t *k = &call_of[core];
        k->arg = arg; xc_barrier(); k->fn = fn; xc_barrier();
        xc_notify(core);
        if (!SPIN_US(!READ_ONCE(k->fn), 10000000u)) {     /* the other core never took it (halted, crashed): no hang */
            k->fn = 0; xc_spin_unlock(&call_lock[core]); return -2;
        }
        r = k->ret;
        xc_spin_unlock(&call_lock[core]);
    }
    if (ret) *ret = r;
    return 0;
}

#if SOC_CPU_RISCV
/* P4: PMU CPU_SW_STALL holds a stall code per core (0x86 stalls, 0xFF runs; core 0 b31:24, core 1 b23:16), IDF cpu_utility_ll */
#include "soc.h"
void xc_stall(int core)   { REG_FIELD(PMU_CPU_SW_STALL, core ? 16 : 24, 8, 0x86); }
void xc_unstall(int core) { REG_FIELD(PMU_CPU_SW_STALL, core ? 16 : 24, 8, 0xFF); }
#else
/* stall: 0x86 split over RTC_CNTL OPTIONS0 (c0, 2 bits) and SW_CPU_STALL (c1, 6 bits), as IDF cpu_utility_ll */
#define OPTIONS0   0x60008000u
#define SW_STALL   0x600080BCu
void xc_stall(int core)
{
    int s0 = core ? 0 : 2, s1 = core ? 20 : 26;
    REG(SW_STALL) = (REG(SW_STALL) & ~(0x3Fu << s1)) | 0x21u << s1;
    REG(OPTIONS0) = (REG(OPTIONS0) & ~(3u << s0)) | 2u << s0;
}
void xc_unstall(int core)
{
    REG(OPTIONS0) &= ~(3u << (core ? 0 : 2));
    REG(SW_STALL) &= ~(0x3Fu << (core ? 20 : 26));
}
#endif
