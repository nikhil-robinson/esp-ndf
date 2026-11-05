/* CPU frequency locks, see pm.h. One counter for both cores under a spinlock with interrupts masked; the frequency
 * changes only on 0 <-> 1 transitions. */
#include "pm.h"
#include "clk.h"
#include "xcore.h"
#include "cpu.h"

static xc_spin_t lk;
static volatile int count;
#ifndef SOC_PM_MHZ_OK                       /* soc_caps.h: the CPU frequencies the clock tree offers */
#define SOC_PM_MHZ_OK(m)  ((m) == 240 || (m) == 160 || (m) == 80)
#define SOC_PM_MHZ_MAX    240
#endif
static uint32_t fmax = SOC_PM_MHZ_MAX, fmin = SOC_PM_MHZ_MAX;

static bool ok_mhz(uint32_t m) { return SOC_PM_MHZ_OK(m); }

int pm_config(uint32_t max_mhz, uint32_t min_mhz)
{
    if (!ok_mhz(max_mhz) || !ok_mhz(min_mhz) || min_mhz > max_mhz) return -1;
    uint32_t ps = irq_lock(); xc_spin_lock(&lk);
    fmax = max_mhz; fmin = min_mhz;
    clk_cpu_set_mhz(count ? fmax : fmin);
    xc_spin_unlock(&lk); irq_unlock(ps);
    return 0;
}

void pm_lock(void)
{
    uint32_t ps = irq_lock(); xc_spin_lock(&lk);
    if (count++ == 0 && fmax != fmin) clk_cpu_set_mhz(fmax);
    xc_spin_unlock(&lk); irq_unlock(ps);
}

void pm_unlock(void)
{
    uint32_t ps = irq_lock(); xc_spin_lock(&lk);
    if (count > 0 && --count == 0 && fmax != fmin) clk_cpu_set_mhz(fmin);
    xc_spin_unlock(&lk); irq_unlock(ps);
}

int pm_locks(void) { return count; }
