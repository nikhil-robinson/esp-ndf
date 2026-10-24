/* Fast-boot loader. The ROM's own image loading runs at ~0.52 us per byte (measured: bench/boot/result.txt), so the
 * ROM loads only this loader (< 1 KB). It maps flash through the MMU + data cache (as drivers/flash/flash.c, keeping
 * the ROM's SPI0 mode), copies the segments of the application image at NDF_BOOT_LOAD_ADDR (standard ESP image:
 * header, then load address + length + data per segment) into RAM through the cache, and jumps to its entry. Same
 * job as IDF's 2nd-stage bootloader (bootloader_utility.c unpack_load_app: bootloader_mmap + memcpy), without
 * image verification (as IDF's BOOTLOADER_SKIP_VALIDATE_ALWAYS). Runs at IRAM 0x403D8000 / DRAM 0x3FCE8000. */
#include <stdint.h>
#include "ndf.h"
#include "soc.h"
#include "regi2c.h"
#define DCACHE_CTRL   0x600C4000u     /* b0 enable, b4:3 line size */
#define DCACHE_CTRL1  0x600C4004u     /* b0 / b1: data bus of core 0 / 1 shut */
#define DCACHE_SYNC   0x600C4028u     /* b0 invalidate, b3 done */
#define DCACHE_SYNC_ADDR 0x600C402Cu
#define DCACHE_SYNC_SIZE 0x600C4030u
#define MMU(i)        (0x600C5000u + 4u * (i))
#define FLASH_DBUS    0x3C000000u
#define S0            0x60003000u     /* SPI0 (the cache's flash controller), as drivers/flash/flash.c */

static void dcache_invalidate(void)
{
    REG(DCACHE_SYNC_ADDR) = 0; REG(DCACHE_SYNC_SIZE) = 0; REG(DCACHE_SYNC) = 1;
    while (!(REG(DCACHE_SYNC) & 8u)) {}
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t sys16(void)                     /* system timer, 16 MHz from reset (low word) */
{
    REG(0x60023004u) = 1u << 30;
    while (!(REG(0x60023004u) & (1u << 29))) {}
    return REG(0x60023044u);
}
/* PLL 480 MHz and the CPU at NDF_CPU_MHZ: the same register steps as soc/esp32s3/clk.c (clk_bbpll_init +
 * clk_cpu_set_mhz), only the path from the ROM's XTAL clock up (the ROM loads every loader byte at ~0.68 us). */
static void spin(uint32_t cycles) { uint32_t t; __asm__ volatile("rsr.ccount %0" : "=r"(t)); for (uint32_t c = t; c - t < cycles; ) __asm__ volatile("rsr.ccount %0" : "=r"(c)); }
static void boot_clock(void)
{
    REG_FIELD(SYSTEM_SYSCLK_CONF, 10, 2, 0);                              /* CPU on XTAL /1 meanwhile */
    REG_FIELD(SYSTEM_SYSCLK_CONF, 0, 10, 0);
    REG_FIELD(SYSTEM_CPU_PER, 2, 1, 1);                                   /* PLL 480 MHz */
#if NDF_LOADER_RTC          /* warm reset: the BBPLL and core voltage (analog, RTC domain) are still set, PLL locked */
    if (!(REG(RTC_OPTIONS0) & (BIT(6) | BIT(8) | BIT(10)))) goto pll_ready;
#endif
    REG_CLR(RTC_OPTIONS0, BIT(6) | BIT(8) | BIT(10));                     /* BBPLL + its i2c powered */
    REG_CLR(I2C_ANA_CONF0, BIT(2)); REG_SET(I2C_ANA_CONF0, BIT(3));       /* calibration start */
    regi2c_write(REGI2C_BBPLL, 4, 0x6B);
    regi2c_write(REGI2C_BBPLL, 2, 5 << 4);
    regi2c_write(REGI2C_BBPLL, 3, 8);
    regi2c_write_mask(REGI2C_BBPLL, 5, 2, 0, 0);
    regi2c_write_mask(REGI2C_BBPLL, 5, 6, 4, 0);
    regi2c_write(REGI2C_BBPLL, 6, 1 << 6 | 3 << 4 | 3);
    regi2c_write_mask(REGI2C_BBPLL, 9, 1, 0, 3);
    while (!(REG(I2C_ANA_CONF0) & BIT(24))) {}
    spin(400);                                                            /* 10 us at 40 MHz */
    REG_CLR(I2C_ANA_CONF0, BIT(3)); REG_SET(I2C_ANA_CONF0, BIT(2));
    if (NDF_CPU_MHZ == 240) {                                             /* core voltage up first (clk.c dig_dbias) */
        regi2c_write_mask(REGI2C_DIG, 4, 4, 0, 28); regi2c_write_mask(REGI2C_DIG, 6, 4, 0, 28); spin(1600);
    }
#if NDF_LOADER_RTC
pll_ready:                  /* the core voltage is analog too: still set */
#endif
    REG_FIELD(RTC_DATE, 13, 6, 0x7 >> (NDF_CPU_MHZ / 80));
    REG_FIELD(SYSTEM_CPU_PER, 0, 2, NDF_CPU_MHZ == 240 ? 2 : NDF_CPU_MHZ == 160 ? 1 : 0);
    REG_FIELD(SYSTEM_SYSCLK_CONF, 10, 2, 1);                              /* CPU on the PLL */
}

#define RESET_STATE   0x60008038u     /* b13: CPU0 reset vector, 1 = ROM (0x40000400), 0 = RTC slow memory (TRM 10.6) */
#ifndef NDF_BOOT_RTC
#define NDF_BOOT_RTC  1                 /* warm resets skip the ROM; esptool's reset (cause 21/22) still reaches it */
#endif
#define RTC_STAGE     0x50000000u
#define QE_NOTE       0x50001FFCu     /* RTC slow memory: "QE" + quad-enable bit, for the warm-reset stage */
#define RTC_FAST      0x600FE000u     /* RTC fast memory: the stage runs here (loader_rtc.ld) */
#define RTC_STAGE_SRC 0x1000u          /* in flash: "RTCB", jump size, stage size, jump, stage (build/<app>/rtcboot.bin) */

static uint32_t spi1_rd(uint8_t cmd)              /* 1-bit command, first data byte back (SPI1 user transaction) */
{
    REG(0x60002008u) &= ~((1u << 13) | (1u << 14) | (1u << 20) | (1u << 23) | (1u << 24));   /* 1-bit read */
    REG(0x60002018u) = 1u << 31 | 1u << 28 | 1u << 6 | 1u << 7;            /* cmd + miso, cs hold/setup */
    REG(0x6000201Cu) = 0;
    REG(0x60002020u) = 7u << 28 | cmd;
    REG(0x60002028u) = 7;                                                  /* 8 bits */
    REG(0x60002000u) = 1u << 18;
    for (uint32_t k = 0; (REG(0x60002000u) & (1u << 18)) && k < 100000; k++) {}
    return REG(0x60002058u);
}

static void to_rom(void)                /* hand this reset to the ROM: vector back, system reset */
{
    REG_SET(RESET_STATE, BIT(13));
    REG_SET(0x60008000u, BIT(31));      /* RTC_CNTL_SW_SYS_RST */
    for (;;) {}
}

#ifndef NDF_BOOT_TRACE
#define NDF_BOOT_TRACE 0
#endif
void loader_main(void)
{
    uint32_t t_entry = sys16();
#if NDF_LOADER_RTC          /* started by the CPU reset vector, no ROM before us: anything but a normal flash boot is
                             * the ROM's job (download mode by strapping or by the USB-JTAG force-download flag) */
    /* only our own warm resets: 3 system / 12 CPU software reset, 7/8 timer-group, 9/16 RTC watchdog. Others (21/22
     * USB-UART/JTAG reset = esptool) go to the ROM. No strap check: GPIO_STRAP reads 0 here, and a BOOT-button download
     * comes with a power-on/EN reset, which sets the vector back to the ROM in hardware */
    uint32_t why = REG(RESET_STATE) & 0x3Fu;
    if (why > 16 || !(((BIT(3) | BIT(7) | BIT(8) | BIT(9) | BIT(12) | BIT(16)) >> why) & 1) || (REG(0x6000812Cu) & 1) ||
        (REG(0x6000703Cu) & (1u << 9)))       /* octal flash (eFuse FLASH_TYPE): the ROM sets that bus up */
        to_rom();
    /* a system reset returns SPI0 and the flash pads to their defaults; the ROM's flash attach would set them. Same
     * state, written here (diffed against the state the ROM leaves): DIO read 0xBB, 24-bit address, 4 dummy clocks,
     * SPI clock = MSPI core clock, cache requests on; CS0/CLK/Q/D pads (GPIO29-32) to function 0, input on, drive 2 */
    REG(S0 + 0x14) = 1u << 31;                                            /* CLOCK: clk_equ_sysclk */
    REG(S0 + 0x08) = 0xAC2008u;                                           /* CTRL: DIO read, WP high, fast read */
    REG(S0 + 0x10) = 0x2C41u;                                             /* CTRL2: CS setup/hold times */
    REG(S0 + 0x18) = 0x200000C0u;                                         /* USER: CS hold/setup, dummy */
    REG(S0 + 0x1C) = 0x5C000003u;                                         /* USER1: 24-bit address, 4 dummy clocks */
    REG(S0 + 0x20) = 0x700000BBu;                                         /* USER2: 8-bit command 0xBB */
    REG(S0 + 0x3C) = 0x5u;                                                /* CACHE_FCTRL: cache requests, user cmd */
    REG(S0 + 0xE0) = 0;                                                   /* DDR off */
    for (uint32_t g = 29; g <= 32; g++) REG(0x60009004u + 4 * g) = 1u << 9 | 2u << 10;
    /* data cache data arrays on their SRAM block, as the ROM leaves it (cache.c does the same for the instruction
     * cache): CACHE_DATAARRAY_CONNECT dcache field [7:4] = 2, SRAM_USAGE_1 bit 3 clear (block to the cache) */
    REG(0x600C1004u) = (REG(0x600C1004u) & ~0xF0u) | 0x20u;
    REG_CLR(0x600C1014u, BIT(3));
    REG(0x600C4148u) = 0xFFFFFFu;                                         /* EXTMEM_CACHE_MMU_OWNER: MMU blocks to the cache */
#endif
    boot_clock();                                                         /* PLL + CPU clock: the copy runs at full speed */
#if NDF_BOOT_TRACE
    REG(0x50001028u) = sys16();
#endif
    REG(DCACHE_CTRL) &= ~1u;
    REG(DCACHE_CTRL) = (REG(DCACHE_CTRL) & ~(3u << 3)) | 2u << 3;          /* 64-byte lines */
    uint32_t first = NDF_BOOT_LOAD_ADDR >> 16, pages = 8;                 /* the image lies within 512 KB */
    for (uint32_t i = first; i < first + pages; i++) REG(MMU(i)) = i;      /* flash page i at 0x3C000000 + 64K*i */
    dcache_invalidate();
    REG(DCACHE_CTRL1) &= ~1u;
    REG(DCACHE_CTRL) |= 1u;
    __asm__ volatile("memw" ::: "memory");                              /* no flash read may move above the enable */

    const volatile uint8_t *img = (const volatile uint8_t *)(FLASH_DBUS + NDF_BOOT_LOAD_ADDR);
    /* QIO (0xEB, 6 dummy clocks) as drivers/flash/flash.c, only if the chip's quad-enable bit is set (read over SPI1
     * first, as IDF's bootloader: a quad command to a chip with QE clear leaves it unreadable until power-off):
     * MXIC/ISSI SR1 bit 6, others SR2 bit 1. WP/HD pads to their SPI function (the ROM boots in DIO, leaves them GPIO) */
    bool qe = false;
    if (REG(0x6000703Cu) & (1u << 9)) goto bus_ready;                     /* octal flash: keep the ROM's SPI0 setup */
#if NDF_LOADER_RTC          /* warm reset: the cold-boot loader's answer, kept in RTC memory (no slow SPI1 reads) */
    if ((REG(QE_NOTE) & ~1u) == 0x51450000u) qe = REG(QE_NOTE) & 1; else
#endif
    {
        uint32_t mfg = spi1_rd(0x9F) & 0xFF;
        qe = mfg == 0xC2 || mfg == 0x9D ? spi1_rd(0x05) & 0x40 : spi1_rd(0x35) & 0x02;
        REG(QE_NOTE) = 0x51450000u | qe;
    }
    if (qe) {
        REG(0x60009004u + 4 * 27) = 1u << 9 | 3u << 10;                   /* SPIHD: input on, drive 3 */
        REG(0x60009004u + 4 * 28) = 1u << 9 | 3u << 10;                   /* SPIWP */
        REG(S0 + 0x08) = (REG(S0 + 0x08) & ~((1u << 14) | (1u << 20) | (1u << 23))) | (1u << 13) | (1u << 24);
        REG(S0 + 0x1C) = (REG(S0 + 0x1C) & ~0x3Fu) | 5;
        REG(S0 + 0x20) = 7u << 28 | 0xEB;
        dcache_invalidate();
    }
bus_ready:
#if NDF_BOOT_TRACE
    REG(0x5000102Cu) = REG(S0 + 0x20); REG(0x50001030u) = sys16();
    { uint32_t c0, c1, t0 = sys16(); __asm__ volatile("rsr %0, ccount" : "=r"(c0)); while (sys16() - t0 < 1600) {}
      __asm__ volatile("rsr %0, ccount" : "=r"(c1)); REG(0x50001034u) = c1 - c0; }
    REG(0x50001038u) = REG(0x600C0060u); REG(0x5000103Cu) = REG(0x600C0010u);
#endif
#if NDF_LOADER_RTC
    if (img[0] != 0xE9) to_rom();                                         /* flash not readable from here: the ROM boots */
#else
    if (img[0] != 0xE9) for (;;) {}                                       /* no image: stay (download mode still works) */
    const uint32_t *rs = (const uint32_t *)(FLASH_DBUS + RTC_STAGE_SRC);  /* install the RTC boot stage for warm resets */
    if (NDF_BOOT_RTC && rs[0] == 0x42435452u && rs[1] && rs[1] <= 0x100u && rs[2] && rs[2] <= 0x2000u) {
        volatile uint32_t *rj = (volatile uint32_t *)RTC_STAGE, *rf = (volatile uint32_t *)RTC_FAST;
        for (uint32_t k = 0; k < rs[1] / 4; k++) rj[k] = rs[3 + k];       /* jump at the reset vector */
        for (uint32_t k = 0; k < rs[2] / 4; k++) rf[k] = rs[3 + rs[1] / 4 + k];   /* the stage */
        REG_CLR(RESET_STATE, BIT(13));                                    /* next warm reset starts there, no ROM */
    } else REG_SET(RESET_STATE, BIT(13));                                 /* no stage: every reset through the ROM */
#endif
    __asm__ volatile("memw" ::: "memory");
    const uint8_t *im = (const uint8_t *)img;
    uint32_t nseg = im[1], entry = rd32(im + 4);
    const uint8_t *p = im + 24;                                          /* header + extended header */
    for (uint32_t s = 0; s < nseg; s++) {
        uint32_t dst = rd32(p), len = rd32(p + 4);
        const uint32_t *src = (const uint32_t *)(p + 8);
        if (dst >= 0x40378000u && dst < 0x403E0000u) dst -= 0x6F0000u;   /* IRAM through its DRAM alias */
        uint32_t *d = (uint32_t *)dst, n = len / 4, k = 0;
        for (; k + 4 <= n; k += 4) { uint32_t a = src[k], b = src[k + 1], c = src[k + 2], e = src[k + 3]; d[k] = a; d[k + 1] = b; d[k + 2] = c; d[k + 3] = e; }
        for (; k < n; k++) d[k] = src[k];
        p += 8 + len;
    }
#if NDF_BOOT_TRACE                                                        /* bench: loader entry / jump times */
    REG(0x50001020u) = t_entry; REG(0x50001024u) = sys16();
#endif
    (void)t_entry;
    __asm__ volatile("memw" ::: "memory");                              /* stores done before the jump */
    ((void (*)(void))entry)();
}
