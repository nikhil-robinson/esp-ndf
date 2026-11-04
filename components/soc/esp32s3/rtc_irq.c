/* Shared RTC_CNTL interrupt dispatch: brownout (bit 9), touch (4, 6-8, 18), ... at level 3. */
#include "rtc_irq.h"
#include "xtensa.h"

#define INT_ENA  0x60008040u
#define INT_ST   0x60008048u
#define INT_CLR  0x6000804Cu
#define SRC_RTC  39
#define SLOTS    4

static struct { uint32_t mask; irq_fn_t fn; void *arg; } slot[SLOTS];
static int line = -1;

RAM_FN static void rtc_isr(void *a)
{
    (void)a;
    uint32_t st = REG(INT_ST);
    for (int i = 0; i < SLOTS; i++) if (slot[i].fn && (st & slot[i].mask)) slot[i].fn(slot[i].arg);
}

int rtc_irq_on(uint32_t mask, irq_fn_t fn, void *arg)
{
    int i = 0;
    while (i < SLOTS && slot[i].fn && slot[i].fn != fn) i++;
    if (i == SLOTS) return -1;
    if (line < 0 && (line = irq_attach(SRC_RTC, 3, rtc_isr, 0)) < 0) return -1;
    uint32_t ps = irq_lock();
    slot[i].mask = mask; slot[i].arg = arg; slot[i].fn = fn;
    REG(INT_CLR) = mask;
    REG_SET(INT_ENA, mask);
    irq_unlock(ps);
    return 0;
}

void rtc_irq_off(uint32_t mask)
{
    REG_CLR(INT_ENA, mask);
    for (int i = 0; i < SLOTS; i++) if (slot[i].mask & mask) { slot[i].mask &= ~mask; if (!slot[i].mask) slot[i].fn = 0; }
}

void rtc_irq_clear(uint32_t mask) { REG(INT_CLR) = mask; }
