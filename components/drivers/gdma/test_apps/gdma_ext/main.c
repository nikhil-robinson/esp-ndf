/* GDMA beyond the basics: PSRAM <-> internal and PSRAM -> PSRAM copies (checked, throughput vs CPU memcpy),
 * priority (two channels copying at once, prio 5 vs 0), owner check and descriptor errors caught + recovered.
 * Sibling: bench/idf_gdma (esp_async_memcpy with PSRAM). */
#include "console.h"
#include "gdma.h"
#include "psram.h"
#include "sys.h"
#include "cpu.h"
#include <string.h>

static uint8_t ibuf[65536] __attribute__((aligned(64))), ibuf2[32768] __attribute__((aligned(64)));
static uint8_t big_a[32768] __attribute__((aligned(64))), big_b[32768] __attribute__((aligned(64)));
static uint32_t us(uint32_t c) { return c / 240; }
static uint32_t mbs(size_t n, uint32_t cyc) { return (uint32_t)((uint64_t)n * 240 / (cyc ? cyc : 1)); }   /* bytes/us = MB/s */
static void fill(uint8_t *p, size_t n, uint32_t seed) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 13 + seed + (i >> 9)); }
static int same(const uint8_t *p, size_t n, uint32_t seed) { for (size_t i = 0; i < n; i++) if (p[i] != (uint8_t)(i * 13 + seed + (i >> 9))) return 0; return 1; }

static volatile int done_a, done_b; static volatile uint32_t t_a, t_b;
static void fin_a(void *x) { (void)x; t_a = ccount(); done_a = 1; }
static void fin_b(void *x) { (void)x; t_b = ccount(); done_b = 1; }

int main(void)
{
    delay_ms(200);
    int fail = 0;
    uint32_t sz = psram_init(2);
    uint8_t *ps = (uint8_t *)PSRAM_VADDR, *ps2 = ps + 0x100000;
    int ch = gdma_alloc(); gdma_connect(ch, GDMA_M2M);
    printf("PSRAM %lu KB, channel %d\n", (unsigned long)(sz >> 10), ch);
    if (!sz) { printf("gdma_ext FAIL done\n"); for (;;) {} }
    const size_t N = 1 << 20;
    /* internal -> PSRAM (1 MB in 64 KB pieces from ibuf), PSRAM -> internal, PSRAM -> PSRAM */
    fill(ibuf, sizeof ibuf, 5);
    uint32_t t = ccount(); int r = 0;
    for (size_t o = 0; o < N; o += sizeof ibuf) r |= gdma_memcpy(ch, ps + o, ibuf, sizeof ibuf);
    uint32_t tw = ccount() - t; int ok1 = !r; for (size_t o = 0; o < N && ok1; o += sizeof ibuf) ok1 = same(ps + o, sizeof ibuf, 5);
    memset(ibuf, 0, sizeof ibuf);
    t = ccount(); r = gdma_memcpy(ch, ibuf, ps + 0x40000, sizeof ibuf); uint32_t tr = ccount() - t;
    int ok2 = !r && same(ibuf, sizeof ibuf, 5);
    t = ccount(); r = gdma_memcpy(ch, ps2, ps, N); uint32_t tpp = ccount() - t;
    int ok3 = !r; for (size_t o = 0; o < N && ok3; o += sizeof ibuf) ok3 = same(ps2 + o, sizeof ibuf, 5);
    /* CPU memcpy for the same moves */
    psram_writeback(ps, N); psram_invalidate(ps, N);
    t = ccount(); for (size_t o = 0; o < N; o += sizeof ibuf) memcpy(ps2 + o, ibuf, sizeof ibuf); psram_writeback(ps2, N); uint32_t cw = ccount() - t;
    psram_invalidate(ps, N);
    t = ccount(); memcpy(ibuf, ps + 0x40000, sizeof ibuf); uint32_t cr = ccount() - t;
    psram_invalidate(ps, N);
    t = ccount(); memcpy(ps2, ps, N); psram_writeback(ps2, N); uint32_t cpp = ccount() - t;
    int okp = ok1 && ok2 && ok3;
    printf("%s PSRAM DMA: write 1 MB %lu us (%lu MB/s), read 64 KB %lu us (%lu MB/s), PSRAM->PSRAM 1 MB %lu us (%lu MB/s) | "
           "CPU memcpy: %lu / %lu / %lu MB/s\n", okp ? "PASS" : "FAIL", (unsigned long)us(tw), (unsigned long)mbs(N, tw),
           (unsigned long)us(tr), (unsigned long)mbs(sizeof ibuf, tr), (unsigned long)us(tpp), (unsigned long)mbs(N, tpp),
           (unsigned long)mbs(N, cw), (unsigned long)mbs(sizeof ibuf, cr), (unsigned long)mbs(N, cpp));
    fail |= !okp;
    /* unaligned PSRAM request refused */
    int okal = gdma_memcpy(ch, ps + 4, ibuf, 64) == -1;
    /* priority: two channels, internal -> internal 32 KB each at once */
    int c2 = gdma_alloc(); gdma_connect(c2, GDMA_M2M);
    uint32_t order[2];
    for (int k = 0; k < 2; k++) {
        gdma_set_priority(ch, k ? 0 : 5); gdma_set_priority(c2, k ? 5 : 0);
        done_a = done_b = 0;
        t = ccount();
        gdma_memcpy_async(ch, big_a, ibuf, sizeof big_a, fin_a, 0, 1);
        gdma_memcpy_async(c2, big_b, ibuf2, sizeof big_b, fin_b, 0, 1);
        while (!done_a || !done_b) ;
        order[k] = (int32_t)(t_a - t_b) < 0 ? 1 : 2;                  /* which channel finished first */
        printf("  prio %s: ch %d done %lu us, ch %d done %lu us\n", k ? "0/5" : "5/0", ch, (unsigned long)us(t_a - t),
               c2, (unsigned long)us(t_b - t));
    }
    int okpr = order[0] == 1 && order[1] == 2;
    gdma_set_priority(ch, 0); gdma_set_priority(c2, 0);
    printf("%s priority: higher-priority channel finished first both ways\n", okpr ? "PASS" : "FAIL");
    fail |= !okpr;
    /* owner check: a CPU-owned descriptor is refused, error reported, channel recovers */
    static gdma_desc_t din[2], dout[2];
    gdma_check_owner(ch, true);
    gdma_chain(din, 2, ibuf2, 256, false); gdma_chain(dout, 2, ibuf, 256, true);
    dout[0].owner = 0;
    gdma_in_start(ch, din); gdma_out_start(ch, dout);
    delay_us(100);
    int e1 = gdma_error(ch);
    gdma_reset(ch); gdma_check_owner(ch, false);
    fill(ibuf, 256, 9);
    int okr = gdma_memcpy(ch, ibuf2, ibuf, 256) == 0 && same(ibuf2, 256, 9);
    /* bad address in a descriptor (peripheral space, refused by PMS): -3, then recovery */
    gdma_chain(din, 2, ibuf2, 256, false); gdma_chain(dout, 2, (void *)0x60000000, 256, true);
    gdma_in_start(ch, din); gdma_out_start(ch, dout); delay_us(100);
    int e2 = gdma_error(ch);
    gdma_reset(ch);
    int okr2 = gdma_memcpy(ch, ibuf2, ibuf, 256) == 0 && same(ibuf2, 256, 9);
    int oke = okal && (e1 & GDMA_ERR_OUT) && okr && e2 && okr2;
    printf("%s errors: unaligned PSRAM refused %d, owner=0 -> error %d, recovered %d; bad address -> error %d, recovered %d\n",
           oke ? "PASS" : "FAIL", okal, e1, okr, e2, okr2);
    fail |= !oke;
    printf("gdma_ext %s done\n", fail ? "FAIL" : "PASS");
    for (;;) {}
}
