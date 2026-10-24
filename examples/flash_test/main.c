/* Flash driver test and benchmark (sibling: bench/idf_flash). Test area: the last 1 MB of the flash.
 * 1. chip: JEDEC ID, SFDP erase types, suspend, quad-enable, unique ID
 * 2. erase 4 / 32 / 64 KB, region 256 KB; write 4 KB / 64 KB; read 4 KB / 64 KB cold; data checked
 * 3. core 1 runs flash code non-stop (each call misses the instruction cache) while core 0 erases 64 KB and writes
 *    4 KB: with suspend, core 1 keeps running (longest pause = one suspend/read/resume); without, it waits.
 * Build with XIP=lib for part 3. DEFS="-DNDF_FLASH_SUSPEND=0|2" for the variants. */
#include "console.h"
#include "flash.h"
#include "cache.h"
#include "sys.h"
#include "xtensa.h"
#include <string.h>

#define AREA (flash_size() - 0x100000)
static uint8_t w[65536], r[65536];

static uint32_t us_since(uint32_t t) { return (ccount() - t) / (sys_cpu_hz() / 1000000); }

/* the same kernel in flash (XIP) and in RAM; noipa: the compiler may not fold or hoist the calls */
#define KERNEL { for (int k = 0; k < 16; k++) x = x * 1664525u + 1013904223u; return x; }
static FLASH_FN __attribute__((noipa)) uint32_t flash_kernel(uint32_t x) KERNEL
static __attribute__((noipa)) uint32_t ram_kernel(uint32_t x) KERNEL

static volatile uint32_t c1_runs, c1_bad, c1_gap, c1_stop, c1_go, c1_sus, c1_every, c1_still;   /* c1_every: us between misses */
static void core1_loop(void)
{
    uint32_t last = ccount();
    while (!c1_stop) {
        if (!c1_go) { last = ccount(); continue; }
        cache_invalidate_code_range((uint32_t)flash_kernel - 0x42000000u, 64);   /* next call fetches from flash */
        uint32_t x = c1_runs;
        if (flash_kernel(x) != ram_kernel(x)) c1_bad++;
        if (REG(0x600020F8u) & 2) { c1_sus++; REG(0x600020F4u) = 2; }   /* SPI1 PES_END: a suspend happened */
        if (REG(0x600020A4u) & 1) c1_still++;                          /* SPI1 SUS_STATUS: still suspended after our read */
        uint32_t now = ccount();
        if (now - last > c1_gap) c1_gap = now - last;
        c1_runs++;
        if (c1_every) { while (ccount() - now < c1_every * 240) {} }   /* RAM spin: no flash access */
        last = ccount();
    }
    for (;;) {}
}

static int check(const uint8_t *a, uint8_t v, size_t n) { for (size_t i = 0; i < n; i++) if (a[i] != v) return 0; return 1; }

int main(void)
{
    delay_ms(300);
    const flash_info_t *f = flash_info();
    { volatile uint32_t *rd = (volatile uint32_t *)0x50001400u;     /* SPI1 / SPI0 registers after init: diff vs bench/idf_flash */
      for (int k = 0; k < 64; k++) { rd[k] = REG(0x60002000u + 4 * k); rd[64 + k] = REG(0x60003000u + 4 * k); } }
    uint8_t uid[8] = { 0 }; flash_unique_id(uid);
    flash_cached(); printf("flash: JEDEC %06lx, %lu KB, SFDP %u dwords, quad %d (QE method %u), suspend %d (%02x/%02x), 4-byte %d, %lu MHz\n",
           (unsigned long)f->jedec_id, (unsigned long)(f->size >> 10), f->sfdp, f->quad, f->qe_method, f->suspend,
           f->sus_cmd, f->res_cmd, f->addr4, (unsigned long)flash_clock_mhz());
    printf("  unique ID %02x%02x%02x%02x%02x%02x%02x%02x\n", uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], uid[7]);
    for (int k = 0; k < 4 && f->erase_size[k]; k++)
        printf("  erase type %d: %lu KB, cmd %02x\n", k, (unsigned long)(f->erase_size[k] >> 10), f->erase_cmd[k]);

    uint32_t a = AREA, t;
    int fail = 0;
    t = ccount(); int e4 = flash_erase(a, 4096); uint32_t te4 = us_since(t);
    t = ccount(); int e32 = flash_erase(a + 0x8000, 0x8000); uint32_t te32 = us_since(t);
    t = ccount(); int e64 = flash_erase(a + 0x10000, 0x10000); uint32_t te64 = us_since(t);
    t = ccount(); int e256 = flash_erase(a + 0x40000, 0x40000); uint32_t te256 = us_since(t);
    flash_read(a, r, 4096); int ok4 = check(r, 0xFF, 4096);
    flash_read(a + 0x10000, r, 65536); int ok64 = check(r, 0xFF, 65536);
    printf("%s erase: 4 KB %lu us, 32 KB %lu us, 64 KB %lu us, 256 KB region %lu us; blank %d/%d\n",
           !e4 && !e32 && !e64 && !e256 && ok4 && ok64 ? "PASS" : "FAIL", (unsigned long)te4, (unsigned long)te32,
           (unsigned long)te64, (unsigned long)te256, ok4, ok64);
    fail |= e4 || e32 || e64 || e256 || !ok4 || !ok64;

    for (size_t i = 0; i < sizeof w; i++) w[i] = (uint8_t)(i * 31 + (i >> 8));
    t = ccount(); int w4 = flash_write(a, w, 4096); uint32_t tw4 = us_since(t);
    t = ccount(); int w64 = flash_write(a + 0x10000, w, 65536); uint32_t tw64 = us_since(t);
    t = ccount(); flash_read(a, r, 4096); uint32_t tr4 = us_since(t);
    int v4 = !memcmp(r, w, 4096);
    t = ccount(); flash_read(a + 0x10000, r, 65536); uint32_t tr64 = us_since(t);
    int v64 = !memcmp(r, w, 65536);
    t = ccount(); flash_read(a + 0x10000, r, 65536); uint32_t tr64h = us_since(t);
    printf("%s write: 4 KB %lu us, 64 KB %lu us; read cold: 4 KB %lu us, 64 KB %lu us (%lu KB/s), 64 KB again %lu us\n",
           !w4 && !w64 && v4 && v64 ? "PASS" : "FAIL", (unsigned long)tw4, (unsigned long)tw64, (unsigned long)tr4,
           (unsigned long)tr64, (unsigned long)(65536ull * 1000000 / 1024 / (tr64 ? tr64 : 1)), (unsigned long)tr64h);
    fail |= w4 || w64 || !v4 || !v64;

    if (cache_xip_active()) {
        uint32_t c0 = ccount();
        for (int k = 0; k < 1000; k++) flash_kernel(k);
        uint32_t hit = (ccount() - c0) / 1000;
        c0 = ccount();
        for (int k = 0; k < 1000; k++) { cache_invalidate_code_range((uint32_t)flash_kernel - 0x42000000u, 64); flash_kernel(k); }
        uint32_t miss = (ccount() - c0) / 1000;
        c0 = ccount();
        for (int k = 0; k < 1000; k++) { cache_invalidate_code_range(0x200000, 64); flash_kernel(k); }
        uint32_t inv = (ccount() - c0) / 1000;
        volatile uint32_t sink = 0;
        c0 = ccount();
        for (int k = 0; k < 1000; k++) { cache_invalidate_code_range((uint32_t)flash_kernel - 0x42000000u, 64); sink += flash_kernel(k); }
        uint32_t miss2 = (ccount() - c0) / 1000;
        c0 = ccount();
        for (int k = 0; k < 1000; k++) { cache_invalidate_code_range(0x200000, 64); sink += flash_kernel(k); }
        uint32_t inv2 = (ccount() - c0) / 1000;
        printf("  flash_kernel at %p: %lu cycles cached, %lu after invalidating its line, %lu invalidating another; "
               "result used: %lu / %lu\n", (void *)flash_kernel, (unsigned long)hit, (unsigned long)miss,
               (unsigned long)inv, (unsigned long)miss2, (unsigned long)inv2);
        cpu1_start(core1_loop);
        delay_ms(20);
        c1_go = 1; delay_ms(20);
        uint32_t i0 = c1_runs; delay_ms(10); printf("  core 1 idle: %lu runs in 10 ms\n", (unsigned long)(c1_runs - i0));
        static const uint32_t every[] = { 0, 20, 50, 100, 200, 500, 1000 };
        for (unsigned m = 0; m < sizeof every / sizeof every[0]; m++) {
            c1_every = every[m]; delay_ms(5);
            uint32_t before = c1_runs; c1_gap = 0; REG(0x600020F4u) = 3; c1_sus = 0; c1_still = 0;
            t = ccount();
            int ee = flash_erase(a + 0x20000, 0x10000), ww = flash_write(a + 0x20000, w, 4096);
            uint32_t top = us_since(t), runs = c1_runs - before, gap = c1_gap / (sys_cpu_hz() / 1000000);
            flash_read(a + 0x20000, r, 4096);
            int vv = !memcmp(r, w, 4096);
            flash_read(a + 0x21000, r, 4096);
            int bl = check(r, 0xFF, 4096);
            int pass = !ee && !ww && vv && bl && !c1_bad && (f->suspend ? runs > 2 : 1);
            printf("%s miss every %4lu us: erase 64 KB + write 4 KB %7lu us; core 1 %5lu runs (%5lu suspends), longest "
                   "pause %5lu us, bad %lu, data %s, still suspended after read %lu\n", pass ? "PASS" : "FAIL", (unsigned long)every[m],
                   (unsigned long)top, (unsigned long)runs, (unsigned long)c1_sus, (unsigned long)gap,
                   (unsigned long)c1_bad, vv && bl ? "ok" : "wrong", (unsigned long)c1_still);
            fail |= !pass;
        }
        c1_stop = 1; delay_ms(5);
    }
    flash_erase(a, 0x100000 - 0x80000);                               /* leave the area blank */
    printf("flash_test %s done\n", fail ? "FAIL" : "PASS");
    for (;;) {}
}
