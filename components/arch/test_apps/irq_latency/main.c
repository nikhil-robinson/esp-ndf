/* Interrupt latency in CPU cycles, per level, using the core's CCOMPARE
 * timers: they fire at an exact CCOUNT, so latency = CCOUNT at handler
 * entry - CCOMPARE. Measures our whole path: vector + context save + C
 * dispatch + call. Compare: Cortex-M4 = 12 cycles to first handler insn. */
#include "sys.h"
#include "irq.h"
#include "console.h"
#include "cpu.h"

#define N 20000

#if SOC_CPU_RISCV
#include "soc.h"
/* RISC-V (P4): no cycle-exact match timer, so the handler is triggered by setting the CLIC line's pending bit;
 * latency = mcycle at the first handler instruction - mcycle just before the trigger store (includes the store). */
#define CLIC_IP(n) (*(volatile uint8_t *)(0x20801000u + 4u * (16 + (n))))
typedef struct { uint32_t min, max, n; uint64_t sum; volatile uint32_t t0, done; } lat_t;
static lat_t lat[3];

#define FROM_CPU0 (0x500E5000u + 0x10)       /* HP_SYSTEM_CPU_INT_FROM_CPU_0 */
static lat_t mtx[2];

RAM_FN static void mtx_isr(void *arg)             /* same path as the IDF twin (bench/idf_p4_irq): matrix source */
{
    uint32_t now = ccount();
    lat_t *l = arg;
    REG(FROM_CPU0) = 0;
    uint32_t d = now - l->t0;
    if (d < l->min) l->min = d;
    if (d > l->max) l->max = d;
    l->sum += d; l->n++; l->done = 1;
}

RAM_FN static void sw_isr(void *arg)
{
    uint32_t now = ccount();
    lat_t *l = arg;
    uint32_t d = now - l->t0;
    if (d < l->min) l->min = d;
    if (d > l->max) l->max = d;
    l->sum += d; l->n++; l->done = 1;
}

int main(void)
{
    static const int line[3] = { 1, 2, 3 }, level[3] = { 1, 3, 5 };
    uint32_t mhz = sys_cpu_hz() / 1000000;
    printf("\nesp-ndf interrupt latency (%d samples per level, cycles @ %lu MHz)\n", N, mhz);
    for (int k = 0; k < 3; k++) {
        lat_t *l = &lat[k];
        l->min = ~0u;
        irq_install(line[k], sw_isr, l);
        volatile uint8_t *c = &CLIC_IP(line[k]);
        c[3] = (uint8_t)CLIC_LVL_BYTE(level[k]);
        irq_enable_all();
        for (int i = 0; i < N; i++) {
            l->done = 0;
            l->t0 = ccount();
            *c = 1;
            while (!l->done) {}
        }
        printf("level %d (line %2d): min %3lu  avg %3lu  max %3lu cycles  = %lu / %lu ns\n", level[k], line[k], l->min,
               (uint32_t)(l->sum / l->n), l->max, l->min * 1000 / mhz, l->max * 1000 / mhz);
    }
    printf("FROM_CPU0 through the interrupt matrix (IDF twin: bench/idf_p4_irq)\n");
    for (int k = 0; k < 2; k++) {
        lat_t *l = &mtx[k];
        l->min = ~0u;
        int n = irq_attach(SRC_FROM_CPU0, k ? 3 : 1, mtx_isr, l);
        for (int i = 0; i < N; i++) {
            l->done = 0;
            l->t0 = ccount();
            REG(FROM_CPU0) = 1;
            while (!l->done) {}
        }
        irq_detach(SRC_FROM_CPU0, n);
        printf("level %d: min %3lu  avg %3lu  max %3lu cycles  = %lu / %lu ns\n", k ? 3 : 1, l->min,
               (uint32_t)(l->sum / l->n), l->max, l->min * 1000 / mhz, l->max * 1000 / mhz);
    }
    return 0;
}
#else

typedef struct { uint32_t min, max, n; uint64_t sum; volatile uint32_t target, done; } lat_t;
static lat_t lat[3];

#define CC_HANDLER(i)                                                       \
RAM_FN static void cc##i##_isr(void *arg)                                  \
{                                                                          \
    uint32_t now = ccount();                                               \
    lat_t *l = arg;                                                        \
    uint32_t d = now - l->target;                                          \
    __asm__ volatile("wsr.ccompare" #i " %0" :: "r"(l->target - 1));      \
    if (d < l->min) l->min = d;                                            \
    if (d > l->max) l->max = d;                                            \
    l->sum += d; l->n++; l->done = 1;                                      \
}
CC_HANDLER(0)
CC_HANDLER(1)
CC_HANDLER(2)

#define ARM(i, l) do {                                                      \
    (l)->done = 0;                                                         \
    (l)->target = ccount() + 2000;                                         \
    __asm__ volatile("wsr.ccompare" #i " %0; rsync" :: "r"((l)->target)); \
} while (0)

int main(void)
{
    static const int cpu_int[3] = { 6, 15, 16 }, level[3] = { 1, 3, 5 };
    irq_fn_t fn[3] = { cc0_isr, cc1_isr, cc2_isr };

    printf("\nesp-ndf interrupt latency (%d samples per level, cycles @ 240 MHz)\n", N);
    for (int k = 0; k < 3; k++) {
        lat_t *l = &lat[k];
        l->min = ~0u;
        uint32_t far = ccount() - 1;     /* clear stale match before enabling */
        if (k == 0) __asm__ volatile("wsr.ccompare0 %0" :: "r"(far));
        if (k == 1) __asm__ volatile("wsr.ccompare1 %0" :: "r"(far));
        if (k == 2) __asm__ volatile("wsr.ccompare2 %0" :: "r"(far));
        irq_install(cpu_int[k], fn[k], l);
        irq_enable_all();
        for (int i = 0; i < N; i++) {
            if (k == 0) ARM(0, l); else if (k == 1) ARM(1, l); else ARM(2, l);
            while (!l->done) {}
        }
        printf("level %d (cpu int %2d): min %3lu  avg %3lu  max %3lu cycles  = %lu / %lu ns\n",
               level[k], cpu_int[k], l->min, (uint32_t)(l->sum / l->n), l->max,
               l->min * 25 / 6, l->max * 25 / 6);
    }
    return 0;
}
#endif
