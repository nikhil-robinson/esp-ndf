/* Cross-core notification: SYSTEM_CPU_INTR_FROM_CPU_n (sources 79 + n) is a
 * level interrupt; the receiving core clears it in its handler. */
#include "xcore.h"
#include "irq.h"

#define FROM_CPU_REG(n)  (0x600C0030 + 4 * (n))
#define SRC_FROM_CPU(n)  (79 + (n))              /* TRM interrupt table: CPU_INTR_FROM_CPU_0 = 79 */

static void (*fn_of[2])(void *);
static void *arg_of[2];

RAM_FN static void notify_isr(void *a)
{
    int c = (int)a;
    REG(FROM_CPU_REG(c)) = 0;                      /* level: clear before running the handler */
    fn_of[c](arg_of[c]);
}

int xc_notify_init(void (*fn)(void *), void *arg, int level)
{
    int c = core_id();
    fn_of[c] = fn; arg_of[c] = arg;
    REG(FROM_CPU_REG(c)) = 0;
    return irq_attach(SRC_FROM_CPU(c), level, notify_isr, (void *)c) < 0 ? -1 : 0;
}
