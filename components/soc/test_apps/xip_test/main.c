/* Code from flash (XIP) test and benchmark.
 * 1. The same 64 generated kernels (bench/xip/xip_kern.c, ~24 KB of code) in RAM and in flash: results must agree;
 *    cycles per call from RAM, from flash warm, from flash cold (instruction cache invalidated first).
 * 2. Flash erase + write while core 1 runs flash code non-stop: data read back, core 1 results still right.
 * Same kernels and measurements in the IDF sibling bench/idf_xip. */
#include "console.h"
#include "sys.h"
#include "cache.h"
#include "flash.h"
#include "xtensa.h"
#include <string.h>

#define XK_ATTR RAM_FN
#define XK_P    xr
#include "xip/xip_kern.c"
#undef XK_ATTR
#undef XK_P
#define XK_ATTR FLASH_FN
#define XK_P    xf
#include "xip/xip_kern.c"

static const char flash_ro[] = "read-only data, read through the data cache";   /* .rodata of an app file: XIP all -> flash */

static uint32_t pass(uint32_t (*const t[])(uint32_t), uint32_t *sum)
{
    uint32_t s = 0, c0 = ccount();
    for (int k = 0; k < XK_COUNT; k++) s += t[k](s + (uint32_t)k);
    uint32_t c = ccount() - c0;
    *sum = s;
    return c;
}

static volatile uint32_t c1_runs, c1_bad, c1_stop;
static void core1_loop(void)                          /* flash code non-stop on core 1 */
{
    uint32_t want; pass(xr_table, &want);
    while (!c1_stop) { uint32_t s; pass(xf_table, &s); c1_runs++; if (s != want) c1_bad++; }
    for (;;) {}
}

int main(void)
{
    delay_ms(300);
    printf("xip_test active %d, %u kernels, flash code %lu bytes\n", cache_xip_active(), XK_COUNT,
           (unsigned long)((uint32_t)xf_table[XK_COUNT - 1] - (uint32_t)xf_table[0]));
    uint32_t sr, sf; pass(xr_table, &sr); pass(xf_table, &sf);
    printf("%s results RAM %08lx flash %08lx\n", sr == sf ? "PASS" : "FAIL", (unsigned long)sr, (unsigned long)sf);
    uint32_t ram = pass(xr_table, &sr), warm = pass(xf_table, &sf);
    cache_invalidate_code();
    uint32_t cold = pass(xf_table, &sf);
    printf("cycles per call: RAM %lu, flash warm %lu, flash cold %lu\n", (unsigned long)(ram / XK_COUNT),
           (unsigned long)(warm / XK_COUNT), (unsigned long)(cold / XK_COUNT));
    printf("%s rodata: \"%s\" at %p\n", strlen(flash_ro) == 43 ? "PASS" : "FAIL", flash_ro, (const void *)flash_ro);

    cpu1_start(core1_loop);
    delay_ms(50);
    uint32_t a = NDF_XIP_ADDR + 0x100000 - 0x1000, r0 = c1_runs;   /* last sector of the xip partition */
    static uint8_t w[256], r[256];
    for (int k = 0; k < 256; k++) w[k] = (uint8_t)(k * 7 + 3);
    uint32_t t0 = ccount(), before = c1_runs;
    int e = flash_erase_sector(a), wr = flash_write(a, w, sizeof w);
    uint32_t us = (ccount() - t0) / 240, during = c1_runs - before;   /* core 1 is stalled: ~0 */
    flash_read(a, r, sizeof r);
    delay_ms(50);
    c1_stop = 1;
    printf("%s flash erase %d write %d readback %s in %lu us; core 1: %lu runs, %lu during the flash operation, bad %lu\n",
           !e && !wr && !memcmp(w, r, sizeof w) && !c1_bad && c1_runs > r0 && during <= 1 ? "PASS" : "FAIL", e, wr,
           memcmp(w, r, sizeof w) ? "wrong" : "ok", (unsigned long)us, (unsigned long)c1_runs, (unsigned long)during,
           (unsigned long)c1_bad);
    printf("xip_test done\n");
    for (;;) {}
}
