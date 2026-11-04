/* A.4: moving the IQ stream out of the dump banks with GDMA while the engine
 * runs. Each 12288-pair unit (48 KiB) is copied by an async GDMA memcpy into
 * a separate SRAM buffer; the copy must finish before the next unit arrives. */
#include "console.h"
#include "cpu.h"
#include "sys.h"
#include "gdma.h"
#include <stdint.h>
#include <string.h>
#include "rf_pll.h"
#include "rf_init.h"
#include "rf_dump.h"

static uint32_t dst[12800] __attribute__((aligned(16)));
static int ch;
static uint32_t late, copies, bad, maxwait;

static uint32_t worst;
static int cpu_cb(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, void *ctx)
{
    (void)ctx; uint32_t t0 = ccount();
    memcpy(dst, a, na * 4);
    if (nb && na + nb <= sizeof dst / 4) memcpy(dst + na, b, nb * 4);
    uint32_t d = ccount() - t0; if (d > worst) worst = d;
    copies++; return 0;
}

static int unit_cb(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, void *ctx)
{
    (void)ctx;
    uint32_t t = ccount();
    while (gdma_busy(ch)) {}                    /* the previous copy must be done by now */
    uint32_t w = ccount() - t;
    if (w > maxwait) maxwait = w;
    if (w > 100) late++;
    if (copies && (dst[0] >> 28) == 0xA) bad++;   /* sentinel pattern: copy did not land */
    gdma_memcpy_async(ch, dst, a, na * 4, NULL, NULL, 0);
    (void)b; (void)nb;                          /* ponytail: head segment only (the wrapped tail is < 1 unit) */
    copies++;
    return 0;
}

int main(void)
{
    delay_ms(1500);
    printf("=== rf_dma\n");
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2437000, &r);
    rf_rx_on();
    ch = gdma_alloc(); gdma_connect(ch, GDMA_M2M);
    uint32_t mhz = sys_cpu_hz() / 1000000;
    /* plain GDMA copy speed, engine idle */
    uint32_t t = ccount(); gdma_memcpy(ch, dst, (const void *)RF_DUMP_BANK(0), 49152); t = ccount() - t;
    printf("gdma 48 KiB SRAM->SRAM, engine idle: %lu us = %.0f MB/s\n", (unsigned long)(t / mhz), 49152.0 * mhz / t);
    t = ccount(); memcpy(dst, (const void *)RF_DUMP_BANK(0), 49152); t = ccount() - t;
    printf("cpu memcpy 48 KiB: %lu us = %.0f MB/s\n", (unsigned long)(t / mhz), 49152.0 * mhz / t);
    static const struct { uint32_t rate; const char *n; } rates[] = { { RF_DUMP_80M, "80" }, { RF_DUMP_40M, "40" }, { RF_DUMP_16M, "16" } };
    for (int k = 0; k < 3; k++) {
        late = copies = bad = maxwait = 0;
        rf_stream_stats_t st;
        uint32_t ps = irq_lock();
        int rc = rf_stream(rates[k].rate, 400, unit_cb, NULL, &st);
        irq_unlock(ps);
        while (gdma_busy(ch)) {}
        printf("stream %s MS/s + GDMA offload: rc %d units %lu copies %lu, copy late %lu, max wait %lu cycles, lost copies %lu, late_max %lu pairs\n",
               rates[k].n, rc, (unsigned long)st.units, (unsigned long)copies, (unsigned long)late, (unsigned long)maxwait, (unsigned long)bad, (unsigned long)st.late_max);
    }
    for (int k = 0; k < 3; k++) {               /* CPU copy instead: memcpy of each unit (both segments) */
        rf_stream_stats_t st; copies = 0;
        uint32_t ps = irq_lock(); worst = 0;
        int rc = rf_stream(rates[k].rate, 1000, cpu_cb, NULL, &st);
        irq_unlock(ps);
        printf("stream %s MS/s + CPU copy: rc %d units %lu, copy max %lu us, late_max %lu pairs\n", rates[k].n, rc,
               (unsigned long)st.units, (unsigned long)(worst / mhz), (unsigned long)st.late_max);
    }
    printf("=== done\n");
    for (;;) {}
}
