/* Probe: reading flash through the data cache + MMU instead of SPI1 registers. */
#include "sys.h"
#include "flash.h"
#include "console.h"
#include "cpu.h"
#include <string.h>

#define DCACHE_CTRL   0x600C4000
#define DCACHE_CTRL1  0x600C4004
#define DCACHE_SYNC   0x600C4028
#define CACHE_STATE   0x600C4130
#define MMU(i)        (0x600C5000 + 4 * (i))

static uint8_t ref[4096] __attribute__((aligned(16))), buf[4096] __attribute__((aligned(16)));

int main(void)
{
    printf("\n=== flash through cache/MMU ===\n");
    printf("  DCACHE_CTRL %08lx CTRL1 %08lx STATE %08lx | MMU[0..3] %08lx %08lx %08lx %08lx\n",
           REG(DCACHE_CTRL), REG(DCACHE_CTRL1), REG(CACHE_STATE), REG(MMU(0)), REG(MMU(1)), REG(MMU(2)), REG(MMU(3)));
    printf("  SPI0 CTRL %08lx USER %08lx USER1 %08lx USER2 %08lx CACHE_FCTRL %08lx CLOCK %08lx\n",
           REG(0x60003008), REG(0x60003018), REG(0x6000301C), REG(0x60003020), REG(0x6000303C), REG(0x60003014));
    flash_read(0x1000, ref, 4096);
    REG(MMU(2)) = 0;                                   /* vaddr 0x3C020000 -> flash page 0, valid, flash */
    REG(DCACHE_SYNC) = BIT(0);                         /* invalidate all */
    uint32_t t = ccount();
    while (!(REG(DCACHE_SYNC) & BIT(3))) if (ccount() - t > (sys_cpu_hz() / 10)) { printf("  invalidate timeout\n"); break; }
    REG_CLR(DCACHE_CTRL1, BIT(0) | BIT(1));            /* dbus on for both cores */
    REG_SET(DCACHE_CTRL, BIT(0));                      /* enable */
    printf("  after enable: DCACHE_CTRL %08lx CTRL1 %08lx\n", REG(DCACHE_CTRL), REG(DCACHE_CTRL1));
    const volatile uint32_t *win = (const volatile uint32_t *)0x3C021000;
    t = ccount();
    for (int i = 0; i < 1024; i++) ((uint32_t *)buf)[i] = win[i];
    uint32_t cold = ccount() - t;
    t = ccount();
    for (int i = 0; i < 1024; i++) ((uint32_t *)buf)[i] = win[i];
    uint32_t warm = ccount() - t;
    printf("  4 KB via cache: cold %lu cycles (%lu us, %lu MB/s), warm %lu cycles; data %s\n",
           cold, cold / (sys_cpu_hz() / 1000000), 4096u * (sys_cpu_hz() / 1000000) / cold, warm, memcmp(buf, ref, 4096) ? "MISMATCH" : "equal to SPI1 read");
    /* switch SPI0 (the cache's flash controller) from DIO to QIO: QE is set (SPI1 uses quad) */
    uint32_t ctrl = REG(0x60003008), user1 = REG(0x6000301C);
    REG(0x60003008) = (ctrl & ~(BIT(14) | BIT(20) | BIT(23))) | BIT(13) | BIT(24);
    REG(0x6000301C) = (user1 & ~0x3Fu) | 5;           /* 6 dummy clocks (QIO: mode bits + 4) */
    REG(0x60003020) = 7u << 28 | 0xEB;                 /* the cache sends the user command: QIO fast read */
    REG(DCACHE_SYNC) = BIT(0);
    while (!(REG(DCACHE_SYNC) & BIT(3))) ;
    t = ccount();
    for (int i = 0; i < 1024; i++) ((uint32_t *)buf)[i] = win[i];
    cold = ccount() - t;
    printf("  QIO: SPI0 CTRL %08lx USER1 %08lx -> 4 KB cold %lu cycles (%lu us, %lu MB/s); data %s\n",
           REG(0x60003008), REG(0x6000301C), cold, cold / (sys_cpu_hz() / 1000000), 4096u * (sys_cpu_hz() / 1000000) / cold, memcmp(buf, ref, 4096) ? "MISMATCH" : "equal");
    /* bigger cache lines: disable, set block size, invalidate, enable */
    for (int bs = 1; bs <= 2; bs++) {
        REG_CLR(DCACHE_CTRL, BIT(0));
        REG(DCACHE_CTRL) = (REG(DCACHE_CTRL) & ~(3u << 3)) | (uint32_t)bs << 3;
        REG(DCACHE_SYNC) = BIT(0);
        while (!(REG(DCACHE_SYNC) & BIT(3))) ;
        REG_SET(DCACHE_CTRL, BIT(0));
        t = ccount();
        for (int i = 0; i < 1024; i++) ((uint32_t *)buf)[i] = win[i];
        cold = ccount() - t;
        printf("  block size mode %d: CTRL %08lx, 4 KB cold %lu cycles (%lu us, %lu MB/s); data %s\n",
               bs, REG(DCACHE_CTRL), cold, cold / (sys_cpu_hz() / 1000000), 4096u * (sys_cpu_hz() / 1000000) / cold, memcmp(buf, ref, 4096) ? "MISMATCH" : "equal");
    }
    /* preload: the cache fetches the whole range itself, then the CPU copies at hit speed */
    REG(DCACHE_SYNC) = BIT(0);
    while (!(REG(DCACHE_SYNC) & BIT(3))) ;
    t = ccount();
    REG(0x600C4044) = 0x3C021000;                       /* PRELOAD_ADDR */
    REG(0x600C4048) = 4096;                             /* PRELOAD_SIZE */
    REG(0x600C4040) = BIT(0);                           /* PRELOAD_ENA */
    while (!(REG(0x600C4040) & BIT(1))) ;               /* PRELOAD_DONE */
    uint32_t tp = ccount() - t;
    for (int i = 0; i < 1024; i++) ((uint32_t *)buf)[i] = win[i];
    cold = ccount() - t;
    printf("  preload 4 KB: preload %lu cycles + copy = %lu cycles (%lu us, %lu MB/s); data %s\n",
           tp, cold, cold / (sys_cpu_hz() / 1000000), 4096u * (sys_cpu_hz() / 1000000) / cold, memcmp(buf, ref, 4096) ? "MISMATCH" : "equal");
    /* autoload: on a miss in the section, the cache prefetches the following lines */
    for (int sz = 0; sz <= 3; sz++) {
        REG(DCACHE_SYNC) = BIT(0);
        while (!(REG(DCACHE_SYNC) & BIT(3))) ;
        REG(0x600C4050) = 0x3C020000;                    /* SCT0 addr */
        REG(0x600C4054) = 0x10000;                       /* SCT0 size: the 64 KB window */
        REG(0x600C404C) = BIT(0) | BIT(2) | (uint32_t)sz << 7;   /* sct0 on, autoload on, size */
        t = ccount();
        for (int i = 0; i < 1024; i++) ((uint32_t *)buf)[i] = win[i];
        cold = ccount() - t;
        printf("  autoload size %d: 4 KB cold %lu cycles (%lu us, %lu MB/s); data %s\n",
               sz, cold, cold / (sys_cpu_hz() / 1000000), 4096u * (sys_cpu_hz() / 1000000) / cold, memcmp(buf, ref, 4096) ? "MISMATCH" : "equal");
    }
    REG(0x600C404C) = 0;
    printf("=== done ===\n");
    for (;;) ;
}
