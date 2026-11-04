/* RISC-V (ESP32-P4) interrupt latency in CPU cycles, per level. Build: make SOC=esp32p4. The S3 version is
 * components/arch/test_apps/irq_latency (CCOMPARE timers). */
#include "sys.h"
#include "irq.h"
#include "console.h"
#include "cpu.h"

#define N 20000

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
