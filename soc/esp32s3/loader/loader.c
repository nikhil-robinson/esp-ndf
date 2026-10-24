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
#if NDF_OTA && !NDF_LOADER_RTC
#define OTA_PENDING ota_pending
#else
#define OTA_PENDING 0
#endif
#define OTA_NOTE      0x50001FF0u     /* RTC slow memory: "OTAS", image offset, its XIP flash offset (cache.c, ota.c) */
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

#if NDF_OTA && !NDF_LOADER_RTC
/* ---- OTA slot choice, ESP-IDF format and rules (bootloader_utility.c, bootloader_common_loader.c, esp_ota_ops.c):
 * partition table at NDF_PART_TABLE_ADDR, otadata = two 32-byte select entries (one per 4 KB sector): seq, label,
 * state, crc32_le(~0, seq). Highest valid seq -> slot (seq - 1) % slots. Rollback: NEW -> image SHA-256 checked once,
 * PENDING_VERIFY; PENDING_VERIFY on the next boot (the app did not confirm) -> ABORTED, fall back. */
enum { ST_NEW = 0, ST_PENDING = 1, ST_VALID = 2, ST_INVALID = 3, ST_ABORTED = 4 };
typedef struct { uint32_t seq; uint8_t label[20]; uint32_t state; uint32_t crc; } ota_sel_t;

static uint32_t crc32_le(uint32_t crc, const uint8_t *p, int n)
{
    crc = ~crc;
    while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (0xEDB88320u & -(crc & 1)); }
    return ~crc;
}
static void spi1_cmd(uint8_t cmd, int abits, uint32_t addr, const uint32_t *w, int nw)   /* 1-bit, data out */
{
    REG(0x60002008u) &= ~((1u << 13) | (1u << 14) | (1u << 20) | (1u << 23) | (1u << 24));
    REG(0x60002018u) = 1u << 31 | (abits ? 1u << 30 : 0) | (nw ? 1u << 27 : 0) | 1u << 6 | 1u << 7;
    REG(0x6000201Cu) = (uint32_t)(abits ? abits - 1 : 0) << 26;
    REG(0x60002020u) = 7u << 28 | cmd;
    REG(0x60002004u) = addr;
    REG(0x60002024u) = nw ? nw * 32 - 1 : 0;
    for (int i = 0; i < nw; i++) REG(0x60002058u + 4 * i) = w[i];
    REG(0x60002000u) = 1u << 18;
    while (REG(0x60002000u) & (1u << 18)) {}
}
static void spi1_idle(void) { while (spi1_rd(0x05) & 1) {} }
static void sel_write(uint32_t at, ota_sel_t *e)                     /* rewrite one select entry (its own sector) */
{
    spi1_idle(); spi1_cmd(0x06, 0, 0, 0, 0); spi1_cmd(0x20, 24, at, 0, 0); spi1_idle();
    spi1_cmd(0x06, 0, 0, 0, 0); spi1_cmd(0x02, 24, at, (const uint32_t *)e, 8); spi1_idle();
    for (uint32_t a = (FLASH_DBUS + at) & ~63u; a < FLASH_DBUS + at + 32; a += 64) {   /* drop the cached copy */
        REG(DCACHE_SYNC_ADDR) = a; REG(DCACHE_SYNC_SIZE) = 64; REG(DCACHE_SYNC) = 1; while (!(REG(DCACHE_SYNC) & 8u)) {}
    }
}
static bool image_ok(uint32_t off)                                    /* appended SHA-256 (hardware engine) */
{
    const uint8_t *im = (const uint8_t *)(FLASH_DBUS + off), *p = im + 24;
    if (im[0] != 0xE9) return false;
    for (uint32_t s = 0, n = im[1]; s < n; s++) p += 8 + rd32(p + 4);
    uint32_t len = ((uint32_t)(p - im) + 16) & ~15u;                  /* padding + checksum byte */
    if (!im[23]) return true;                                         /* no hash appended: header check only */
    REG(0x600C001Cu) |= 1u << 2; REG(0x600C0024u) &= ~((1u << 2) | (1u << 4) | (1u << 5));   /* SHA clock, out of reset */
    const uint8_t *q = im; uint32_t left = len, w[16]; bool first = true, padded = false;
    for (;;) {
        uint32_t n = left < 64 ? left : 64;
        for (int i = 0; i < 16; i++) w[i] = 0;
        for (uint32_t i = 0; i < n; i++) ((uint8_t *)w)[i] = q[i];
        q += n; left -= n;
        bool last = false;
        if (n < 64) {
            if (!padded) { ((uint8_t *)w)[n] = 0x80; padded = true; }
            if (n < 56) { uint32_t lo = len << 3, hi = len >> 29;            /* bit length, big-endian, no 64-bit math */
                for (int i = 0; i < 4; i++) { ((uint8_t *)w)[63 - i] = (uint8_t)(lo >> (8 * i)); ((uint8_t *)w)[59 - i] = (uint8_t)(hi >> (8 * i)); }
                last = true; }
        }
        REG(0x6003B000u) = 2;                                         /* SHA-256 */
        for (int i = 0; i < 16; i++) REG(0x6003B080u + 4 * i) = w[i];
        REG(first ? 0x6003B010u : 0x6003B014u) = 1; first = false;
        while (REG(0x6003B018u)) {}
        if (last) break;
    }
    for (int i = 0; i < 8; i++) if (REG(0x6003B040u + 4 * i) != rd32(im + len + 4 * i)) return false;
    return true;
}
static bool ota_pending;              /* booting an unconfirmed image: next reset must come back here (rollback) */
static uint32_t choose_image(uint32_t *xip)                          /* flash offset of the image to boot */
{
    const uint8_t *pt = (const uint8_t *)(FLASH_DBUS + NDF_PART_TABLE_ADDR);
    uint32_t factory = NDF_BOOT_LOAD_ADDR, otad = 0, slot[16], ns = 0;
    ota_pending = false;                                              /* .bss is not cleared in the loader */
    for (int i = 0; i < 95; i++, pt += 32) {
        if (pt[0] != 0xAA || pt[1] != 0x50) break;
        uint32_t o = rd32(pt + 4);
        if (pt[2] == 0 && pt[3] >= 0x10 && pt[3] < 0x20 && ns < 16) slot[ns++] = o;
        else if (pt[2] == 1 && pt[3] == 0) otad = o;
    }
    *xip = NDF_XIP_ADDR;
    if (!otad || !ns) return factory;
    for (;;) {
        ota_sel_t e[2]; int best = -1;
        for (int k = 0; k < 2; k++) {
            const uint8_t *src = (const uint8_t *)(FLASH_DBUS + otad + 0x1000 * k);
            for (int i = 0; i < 32; i++) ((uint8_t *)&e[k])[i] = src[i];
            bool ok = e[k].seq != 0xFFFFFFFFu && e[k].state != ST_INVALID && e[k].state != ST_ABORTED &&
                      e[k].crc == crc32_le(0xFFFFFFFFu, (const uint8_t *)&e[k].seq, 4);
            if (ok && (best < 0 || e[k].seq > e[best].seq)) best = k;
        }
        if (best < 0) return factory;
        uint32_t at = otad + 0x1000 * best, img = slot[(e[best].seq - 1) % ns];
        if (NDF_OTA_ROLLBACK && e[best].state == ST_PENDING) { e[best].state = ST_ABORTED; sel_write(at, &e[best]); continue; }
        if (e[best].state == ST_NEW) {
            if (!image_ok(img)) { e[best].state = ST_INVALID; sel_write(at, &e[best]); continue; }
            if (NDF_OTA_ROLLBACK) { e[best].state = ST_PENDING; sel_write(at, &e[best]); ota_pending = true; }
        }
        if (((const uint8_t *)(FLASH_DBUS + img))[0] != 0xE9) { e[best].state = ST_INVALID; sel_write(at, &e[best]); continue; }
        *xip = img + NDF_OTA_XIP_OFF;
        return img;
    }
}
#endif

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
#if NDF_LOADER_RTC          /* warm: only the pages of the image to copy (it lies within 512 KB) */
    uint32_t pg = (REG(OTA_NOTE) == 0x5341544Fu ? REG(OTA_NOTE + 4) : NDF_BOOT_LOAD_ADDR) >> 16;
    for (uint32_t i = pg; i < pg + 8; i++) REG(MMU(i)) = i;
#else                       /* cold: the first 16 MB (partition table, otadata, slots) */
    for (uint32_t i = 0; i < 256; i++) REG(MMU(i)) = i;                   /* flash page i at 0x3C000000 + 64K*i */
#endif
    dcache_invalidate();
    REG(DCACHE_CTRL1) &= ~1u;
    REG(DCACHE_CTRL) |= 1u;
    __asm__ volatile("memw" ::: "memory");                              /* no flash read may move above the enable */

    uint32_t img_off = NDF_BOOT_LOAD_ADDR;
#if NDF_LOADER_RTC          /* warm reset: the image the cold path chose (its note), if the cold path left one */
    if (REG(OTA_NOTE) == 0x5341544Fu) img_off = REG(OTA_NOTE + 4);
#endif
    const volatile uint8_t *img = (const volatile uint8_t *)(FLASH_DBUS + img_off);
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
#if NDF_OTA
    {   uint32_t xip, o = choose_image(&xip);
        if (o != NDF_BOOT_LOAD_ADDR) { REG(OTA_NOTE) = 0x5341544Fu; REG(OTA_NOTE + 4) = o; REG(OTA_NOTE + 8) = xip; img = (const volatile uint8_t *)(FLASH_DBUS + o); }
        else REG(OTA_NOTE) = 0;
        if (img[0] != 0xE9) for (;;) {}
    }
#else
    REG(OTA_NOTE) = 0;
#endif
    const uint32_t *rs = (const uint32_t *)(FLASH_DBUS + RTC_STAGE_SRC);  /* install the RTC boot stage for warm resets */
    if (NDF_BOOT_RTC && !OTA_PENDING && rs[0] == 0x42435452u && rs[1] && rs[1] <= 0x100u && rs[2] && rs[2] <= 0x2000u) {
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
