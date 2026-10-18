/* SPI NOR via SPI1 user commands (TRM ch. 30, SPI_MEM). 1-bit transfers,
 * up to 64 data bytes per transaction through W0..W15. */
#include "flash.h"
#include "xtensa.h"
#include "sys.h"
#include <string.h>

#define S1         0x60002000
#define CMD        (S1 + 0x00)   /* b18 usr start */
#define ADDR       (S1 + 0x04)
#define CTRL       (S1 + 0x08)   /* dual/quad read mode bits: cleared for 1-bit commands */
#define USER       (S1 + 0x18)   /* b31 cmd b30 addr b29 dummy b28 miso b27 mosi */
#define USER1      (S1 + 0x1C)   /* b31:26 addr bitlen-1 */
#define USER2      (S1 + 0x20)   /* b15:0 cmd value b31:28 cmd bitlen-1 */
#define MOSI_DLEN  (S1 + 0x24)
#define MISO_DLEN  (S1 + 0x28)
#define W(i)       (S1 + 0x58 + 4 * (i))

#define MODE_MASK  (BIT(13) | BIT(14) | BIT(20) | BIT(23) | BIT(24))   /* fastrd, dual, quad, dio, qio */

/* lines: 1 = plain, 2 = dual-output data, 4 = quad-output data (both with 8 dummy clocks) */
static int op_lines(uint8_t cmd, int addr_bits, uint32_t addr, const uint8_t *out, size_t nout,
                    uint8_t *in, size_t nin, int lines)
{
    uint32_t ctrl = REG(CTRL);
    REG(CTRL) = (ctrl & ~MODE_MASK) | (lines == 2 ? BIT(13) | BIT(14) : lines == 4 ? BIT(13) | BIT(20) : 0);
    uint32_t user = BIT(31) | (addr_bits ? BIT(30) : 0) | (nout ? BIT(27) : 0) | (nin ? BIT(28) : 0) |
                    (lines > 1 ? BIT(29) : 0);
    REG(USER) = user | BIT(6) | BIT(7);                        /* cs hold/setup */
    REG(USER1) = (addr_bits ? addr_bits - 1 : 0) << 26 | (lines > 1 ? 7 : 0);   /* 8 dummy clocks */
    REG(USER2) = cmd | 7u << 28;
    REG(ADDR) = addr;                                          /* plain address (not left-aligned) */
    REG(MOSI_DLEN) = nout ? nout * 8 - 1 : 0;
    REG(MISO_DLEN) = nin ? nin * 8 - 1 : 0;
    for (size_t i = 0; i < (nout + 3) / 4; i++) {
        uint32_t v = 0;
        for (int b = 0; b < 4 && i * 4 + b < nout; b++) v |= (uint32_t)out[i * 4 + b] << (8 * b);
        REG(W(i)) = v;
    }
    REG(CMD) = BIT(18);
    uint32_t t = ccount();
    while (REG(CMD) & BIT(18)) if (ccount() - t > 2400000) { REG(CTRL) = ctrl; return -1; }
    for (size_t i = 0; i < nin; i++) in[i] = REG(W(i / 4)) >> (8 * (i & 3));
    REG(CTRL) = ctrl;
    return 0;
}

static int op(uint8_t cmd, int addr_bits, uint32_t addr, const uint8_t *out, size_t nout, uint8_t *in, size_t nin)
{
    return op_lines(cmd, addr_bits, addr, out, nout, in, nin, 1);
}

static int read_lines;                          /* chosen on first read */

static int wait_ready(uint32_t ms)
{
    uint32_t t = ccount(), lim = ms * (sys_cpu_hz() / 1000);
    uint8_t sr;
    do {
        if (op(0x05, 0, 0, 0, 0, &sr, 1)) return -1;
        if (!(sr & 1)) return 0;
    } while (ccount() - t < lim);
    return -2;
}

uint32_t flash_jedec_id(void)
{
    uint8_t id[3] = { 0 };
    op(0x9F, 0, 0, 0, 0, id, 3);
    return id[0] << 16 | id[1] << 8 | id[2];
}

uint32_t flash_size(void)
{
    uint8_t c = flash_jedec_id() & 0xFF;
    return c >= 0x10 && c <= 0x20 ? 1u << c : 0;
}

static void detect_lines(void)
{
    if (read_lines) return;                     /* quad only if the flash has QE set */
    uint8_t sr2 = 0;
    op(0x35, 0, 0, 0, 0, &sr2, 1);
    read_lines = (sr2 & 0x02) ? 4 : 2;
    if (read_lines == 4) {                       /* WP/HD pads to their SPI function (IO_MUX func 0) */
        REG(0x60009004 + 4 * 27) = BIT(9) | 3u << 10;   /* SPIHD: input on, drive 3 */
        REG(0x60009004 + 4 * 28) = BIT(9) | 3u << 10;   /* SPIWP */
    }
}

/* ---- reads through the data cache (SPI0 + MMU), the fast path ----------------
 * The whole flash is mapped once at 0x3C000000 (64 KB MMU pages). SPI0, the
 * cache's flash controller, is switched from the ROM's DIO to QIO (0xEB, 6
 * dummy clocks) and the cache to 64-byte lines: 4 KB cold in ~131 us
 * (SPI1 register path: 209 us), repeated reads are cache hits. Writes and
 * erases go through SPI1 and invalidate the cache afterwards. */
#define DCACHE_CTRL   0x600C4000      /* b0 enable b3..4 block size (2 = 64 B) */
#define DCACHE_CTRL1  0x600C4004      /* b0/b1 shut dbus core0/core1 */
#define DCACHE_SYNC   0x600C4028      /* b0 invalidate b3 done */
#define MMU(i)        (0x600C5000 + 4 * (i))
#define S0            0x60003000
#define FLASH_VADDR   0x3C000000u
static int mm;                                  /* 0 unknown, 1 ready, -1 not available */

#define DCACHE_SYNC_ADDR  0x600C402C
#define DCACHE_SYNC_SIZE  0x600C4030

static void dcache_sync(void)
{
    REG(DCACHE_SYNC) = BIT(0);
    uint32_t t = ccount();
    while (!(REG(DCACHE_SYNC) & BIT(3)) && ccount() - t < 2400000) {}
}

static void dcache_invalidate(void)                  /* everything */
{
    REG(DCACHE_SYNC_ADDR) = 0; REG(DCACHE_SYNC_SIZE) = 0;
    dcache_sync();
}

static void dcache_invalidate_range(uint32_t addr, size_t len)   /* only lines covering flash [addr, addr+len) */
{
    uint32_t a = (FLASH_VADDR + addr) & ~63u;
    REG(DCACHE_SYNC_ADDR) = a;
    REG(DCACHE_SYNC_SIZE) = ((FLASH_VADDR + addr + len + 63) & ~63u) - a;
    dcache_sync();
}

static void mm_init(void)
{
    detect_lines();
#if SOC_HAS_FLASH_CACHE
    if (read_lines == 4) {
        REG(S0 + 0x08) = (REG(S0 + 0x08) & ~(BIT(14) | BIT(20) | BIT(23))) | BIT(13) | BIT(24);   /* QIO */
        REG(S0 + 0x1C) = (REG(S0 + 0x1C) & ~0x3Fu) | 5;                                         /* 6 dummy clocks */
        REG(S0 + 0x20) = 7u << 28 | 0xEB;                                                       /* cache command */
        REG_CLR(DCACHE_CTRL, BIT(0));
        REG(DCACHE_CTRL) = (REG(DCACHE_CTRL) & ~(3u << 3)) | 2u << 3;                           /* 64-byte lines */
        uint32_t pages = flash_size() >> 16;
        if (pages > 256) pages = 256;
        for (uint32_t i = 0; i < pages; i++) REG(MMU(i)) = i;                                  /* flash page i, valid */
        dcache_invalidate();
        REG_CLR(DCACHE_CTRL1, BIT(0) | BIT(1));
        REG_SET(DCACHE_CTRL, BIT(0));
        mm = 1;
        return;
    }
#endif
    mm = -1;
}

static int spi1_read(uint32_t addr, void *buf, size_t len);

int flash_read(uint32_t addr, void *buf, size_t len)
{
    if (!mm) mm_init();
    if (mm > 0 && addr + len <= flash_size() && addr + len <= 16u << 20) {
        memcpy(buf, (const void *)(FLASH_VADDR + addr), len);   /* cache-line bursts, hits on repeats */
        return 0;
    }
    return spi1_read(addr, buf, len);
}

static int spi1_read(uint32_t addr, void *buf, size_t len)
{
    detect_lines();
    /* Quad: QIO read (0xEB, address on 4 lines, 6 dummy clocks). 32-byte
     * transactions alternate between the low (W0-7) and high (W8-15) data
     * buffer halves, so draining one half overlaps the next transfer. */
    uint32_t ctrl = REG(CTRL);
    bool q = read_lines == 4;
    REG(CTRL) = (ctrl & ~MODE_MASK) | BIT(13) | (q ? BIT(24) : BIT(14));
    REG(USER1) = 23u << 26 | (q ? 5 : 7);                    /* 24-bit address; 6 (QIO) or 8 dummy clocks */
    REG(USER2) = (q ? 0xEB : 0x3B) | 7u << 28;
    const uint32_t user = BIT(31) | BIT(30) | BIT(29) | BIT(28) | BIT(6) | BIT(7);
    uint8_t *b = buf;
    int rc = 0;
    size_t n = len > 32 ? 32 : len;
    int half = 0;                                            /* 0: W0-7, 1: W8-15 */
    REG(USER) = user;
    REG(ADDR) = addr;                                        /* plain address (24-bit field); QIO mode bits ride in the dummy clocks */
    REG(MISO_DLEN) = n * 8 - 1;
    REG(CMD) = BIT(18);
    while (len) {
        uint32_t t = ccount();
        while (REG(CMD) & BIT(18)) if (ccount() - t > 2400000) { rc = -1; goto out; }
        size_t done_n = n;
        int done_half = half;
        addr += n; len -= n;
        if (len) {                                           /* next transfer into the other half */
            n = len > 32 ? 32 : len;
            half ^= 1;
            REG(USER) = user | (half ? BIT(24) : 0);
            REG(ADDR) = addr;
            REG(MISO_DLEN) = n * 8 - 1;
            REG(CMD) = BIT(18);
        }
        uint32_t w0 = done_half * 8;                         /* drain the finished half meanwhile */
        if (!((uintptr_t)b & 3) && done_n == 32) {
            for (int i = 0; i < 8; i++) ((uint32_t *)b)[i] = REG(W(w0 + i));
        } else {
            for (size_t i = 0; i < done_n; i++) b[i] = REG(W(w0 + i / 4)) >> (8 * (i & 3));
        }
        b += done_n;
    }
out:
    REG(CTRL) = ctrl;
    return rc;
}

int flash_erase_sector(uint32_t addr)
{
    if (addr >= 16u << 20) return -1;
    if (wait_ready(10) || op(0x06, 0, 0, 0, 0, 0, 0) || op(0x20, 24, addr & ~(FLASH_SECTOR - 1), 0, 0, 0, 0)) return -1;
    int r = wait_ready(500);                                  /* datasheet max ~400 ms */
    if (mm > 0) dcache_invalidate_range(addr & ~(FLASH_SECTOR - 1), FLASH_SECTOR);   /* cached old contents */
    return r;
}

int flash_write(uint32_t addr, const void *buf, size_t len)
{
    if (addr + len > 16u << 20) return -1;
    uint32_t a0 = addr; size_t l0 = len;
    const uint8_t *b = buf;
    while (len) {
        /* stay inside one 256-byte page and one 64-byte transaction */
        size_t n = FLASH_PAGE - (addr & (FLASH_PAGE - 1));
        if (n > 64) n = 64;
        if (n > len) n = len;
        if (op(0x06, 0, 0, 0, 0, 0, 0) || op(0x02, 24, addr, b, n, 0, 0) || wait_ready(10)) { if (mm > 0) dcache_invalidate_range(a0, l0); return -1; }
        addr += n; b += n; len -= n;
    }
    if (mm > 0) dcache_invalidate_range(a0, l0);
    return 0;
}
int flash_read_lines(void) { return read_lines; }
