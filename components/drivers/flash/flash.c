/* SPI NOR via SPI1 user commands (TRM ch. 30, SPI_MEM), up to 64 data bytes per transaction through W0..W15.
 * Chip support follows ESP-IDF's list (spi_flash_chip_*.c, bootloader flash_qio_mode.c / bootloader_flash.c):
 * generic/XMC, GD, ISSI, MXIC, Winbond, Boya, TH, FM. Per chip: quad-enable bit and how to write it, boot unlock of
 * the protection bits, erase/program suspend, 4-byte addresses above 16 MB, the XMC startup flow. Erase types and
 * suspend also come from the chip's own SFDP table when it has one. */
#include "flash.h"
#include "cache.h"
#include "xtensa.h"
#include "sys.h"
#include <string.h>

#define S1         0x60002000
#define CMD        (S1 + 0x00)   /* b18 usr start, b17 program/erase (auto wait-idle, auto suspend) */
#define ADDR       (S1 + 0x04)
#define CTRL       (S1 + 0x08)   /* dual/quad read mode bits: cleared for 1-bit commands */
#define CTRL1      (S1 + 0x0C)   /* b11:2 CS hold delay before resume (suspend) */
#define USER       (S1 + 0x18)   /* b31 cmd b30 addr b29 dummy b28 miso b27 mosi */
#define USER1      (S1 + 0x1C)   /* b31:26 addr bitlen-1, b5:0 dummy clocks-1 */
#define USER2      (S1 + 0x20)   /* b15:0 cmd value b31:28 cmd bitlen-1 */
#define MOSI_DLEN  (S1 + 0x24)
#define MISO_DLEN  (S1 + 0x28)
#define MISC       (S1 + 0x34)   /* b11 auto resume */
#define W(i)       (S1 + 0x58 + 4 * (i))
#define WAITI_CTRL (S1 + 0x98)   /* b0 auto wait-idle, b9:2 its status command */
#define SUS_CMD    (S1 + 0x9C)   /* b4 suspend on SPI0 request, b3/b2 wait idle after suspend/resume */
#define SUS_CTRL   (S1 + 0xA0)   /* b16:9 suspend cmd, b8:1 resume cmd, b0 auto suspend */
#define SUS_STATUS (S1 + 0xA4)   /* b0 suspended, b6/b5 suspend/resume delay unit 256 clocks */

#define MODE_MASK  (BIT(13) | BIT(14) | BIT(20) | BIT(23) | BIT(24))   /* fastrd, dual, quad, dio, qio */
static uint32_t xdummy;                         /* extra dummy clocks from 120 MHz timing tuning (0 at 80 MHz) */

/* lines: 1 = plain, 2 = dual-output data, 4 = quad-output data; dummy = clocks between address and data */
static bool oct;                                /* MXIC octal flash switched to OPI STR (all phases on 8 lines) */
#define OCT_CTRL   (BIT(13) | BIT(9) | BIT(6) | BIT(5) | BIT(4))   /* fastrd, cmd/addr/din/dout octal (ROM OPI STR) */
#define OCT_MASK   (MODE_MASK | BIT(4) | BIT(5) | BIT(6) | BIT(7) | BIT(8) | BIT(9))
#define CMD16(c)   ((uint32_t)(c) | (uint32_t)(uint8_t)~(c) << 8)  /* MXIC OPI: command byte, then its inverse */

static int op_x(uint8_t cmd, int addr_bits, uint32_t addr, const uint8_t *out, size_t nout,
                uint8_t *in, size_t nin, int lines, int dummy)
{
    uint32_t ctrl = REG(CTRL);
    if (oct) {                                 /* OPI: 32-bit address on every addressed or reading command */
        if (nin && !addr_bits) { addr_bits = 32; addr = 0; dummy = 4; }
        if (addr_bits) addr_bits = 32;
    }
    REG(CTRL) = oct ? (ctrl & ~OCT_MASK) | OCT_CTRL :
                (ctrl & ~MODE_MASK) | (lines == 2 ? BIT(13) | BIT(14) : lines == 4 ? BIT(13) | BIT(20) : 0);
    uint32_t user = BIT(31) | (addr_bits ? BIT(30) : 0) | (nout ? BIT(27) : 0) | (nin ? BIT(28) : 0) |
                    (dummy ? BIT(29) : 0);
    REG(USER) = user | BIT(6) | BIT(7);                        /* cs hold/setup */
    REG(USER1) = (addr_bits ? addr_bits - 1 : 0) << 26 | (dummy ? dummy - 1 + xdummy : 0);
    REG(USER2) = oct ? CMD16(cmd) | 15u << 28 : cmd | 7u << 28;
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
    return op_x(cmd, addr_bits, addr, out, nout, in, nin, 1, 0);
}
static int cmd0(uint8_t cmd) { return op(cmd, 0, 0, 0, 0, 0, 0); }
static uint8_t rd8(uint8_t cmd) { uint8_t v = 0; op(cmd, 0, 0, 0, 0, &v, 1); return v; }

static int read_lines;                          /* 4 = QE set (quad), 2 = dual; chosen at init */

static int wait_ready(uint32_t ms)
{
    uint32_t t = ccount(), lim = ms * (sys_cpu_hz() / 1000);
    do {
        if (!(rd8(0x05) & 1)) return 0;
    } while (ccount() - t < lim);
    return -2;
}

int flash_read_raw(uint32_t addr, void *buf, size_t len)   /* 1-bit 0x03 reads, no driver state (panic path) */
{
    uint8_t *b = buf;
    while (len) {
        size_t n = len > 64 ? 64 : len;
        if (op(0x03, 24, addr, 0, 0, b, n)) return -1;
        addr += n; b += n; len -= n;
    }
    return 0;
}

uint32_t flash_jedec_id(void)
{
    uint8_t id[3] = { 0 };
    op(0x9F, 0, 0, 0, 0, id, 3);
    return id[0] << 16 | id[1] << 8 | id[2];
}

/* ---- chip table (IDF's support list) ------------------------------------------ */
enum { QE_SR2_31, QE_SR2_16, QE_SR1_B6, QE_XMC_OTP };
#define F_SUSPEND 1                             /* hardware auto-suspend validated (IDF caps) */
static const struct { uint8_t mfg; uint16_t id, mask; uint8_t qe, flags; } chips[] = {
    { 0xC2, 0x2000, 0xFF00, QE_SR1_B6,  0 },           /* MXIC */
    { 0x9D, 0x4000, 0xCF00, QE_SR1_B6,  0 },           /* ISSI 0x40xx, 0x70xx */
    { 0xEF, 0x4000, 0xFF00, QE_SR2_16,  0 },           /* Winbond */
    { 0xC8, 0x6000, 0xFF00, QE_SR2_16,  0 },           /* GD25LQ */
    { 0xC8, 0x4017, 0xFFFF, QE_SR2_31,  F_SUSPEND },   /* GD25Q64 */
    { 0xC8, 0x4018, 0xFFFF, QE_SR2_31,  F_SUSPEND },   /* GD25Q128 */
    { 0xC8, 0x4010, 0xFFF0, QE_SR2_16,  0 },           /* GD25Q up to 16 Mbit (0x4010-0x4015): 16-bit status write */
    { 0x20, 0x3817, 0xFFFF, QE_XMC_OTP, 0 },           /* XM25QU64A: QE via OTP mode */
    { 0xCD, 0x6000, 0xFF00, QE_SR2_16,  0 },           /* TH */
    { 0x46, 0x0000, 0x0000, QE_SR2_31,  F_SUSPEND },   /* XMC-D */
    { 0xA1, 0x0000, 0x0000, QE_SR2_31,  F_SUSPEND },   /* FM */
    /* Boya BY25Q: no suspend. Tested (components/drivers/flash/test_apps/flash_test): hardware auto-resume never fires (IDF with suspend forced
     * hangs the same way), and an erase suspended even once by software never completes. Default entry below. */
    { 0x00, 0x0000, 0x0000, QE_SR2_31,  0 },           /* default (GD25Q, Boya BY25Q, FM25Q, XMC ...): SR2 via 0x31 */
};

static flash_info_t info;
static bool info_done;

static int sfdp_read(uint32_t addr, void *buf, size_t len)
{
    return op_x(0x5A, 24, addr, 0, 0, buf, len, 1, 8);
}

/* XMC startup flow (IDF bootloader_flash_xmc_startup): an XM25QHxxC may come up unresponsive; deep power-down,
 * ultra-deep power-down, exit, release brings it back. Run when the ID is not a known-good XMC one but SFDP says XMC. */
static void xmc_startup(void)
{
    uint32_t id = flash_jedec_id(), m = (id >> 8) & 0xFF, c = id & 0xFF;
    bool strict = (id >> 16) == 0x20 && ((m == 0x40 && c >= 0x13 && c <= 0x20) || (m == 0x41 && c >= 0x17 && c <= 0x20) ||
                                         (m == 0x50 && c >= 0x15 && c <= 0x16));
    uint8_t mf = 0;
    if (strict || sfdp_read(0x10, &mf, 1) || mf != 0x20) return;
    cmd0(0xB9); cmd0(0x79); cmd0(0xFF);
    delay_us(2000);
    cmd0(0xAB);
    delay_us(20);
}

static uint32_t qe_read(void)                   /* status as the chip's QE method sees it; QE bit in qe_bit() */
{
    switch (info.qe_method) {
    case QE_SR2_16:  return rd8(0x05) | rd8(0x35) << 8;
    case QE_SR1_B6:  return rd8(0x05);
    case QE_XMC_OTP: { cmd0(0x3A); wait_ready(10); uint32_t v = rd8(0x05); cmd0(0x04); return v; }
    default:         return rd8(0x35);
    }
}
static uint32_t qe_bit(void)
{
    return info.qe_method == QE_SR2_16 ? BIT(9) : info.qe_method == QE_SR2_31 ? BIT(1) : BIT(6);
}
static int qe_write(uint32_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    cmd0(0x06);
    switch (info.qe_method) {
    case QE_SR2_16:  op(0x01, 0, 0, b, 2, 0, 0); break;
    case QE_SR1_B6:  op(0x01, 0, 0, b, 1, 0, 0); break;
    case QE_XMC_OTP: cmd0(0x3A); wait_ready(10); op(0x01, 0, 0, b, 1, 0, 0); wait_ready(50); cmd0(0x04); break;
    default:         op(0x31, 0, 0, b, 1, 0, 0); break;
    }
    return wait_ready(50);
}

/* boot unlock of the block-protection bits, as IDF bootloader_flash_unlock (write only if something changes) */
static void unlock(void)
{
    uint32_t id = info.jedec_id, mfg = id >> 16;
    if (mfg == 0x9D || mfg == 0xC2) {                                  /* ISSI, MXIC: SR1 BP bits + SRWD */
        uint8_t s = rd8(0x05), n = s & ~(BIT(7) | BIT(5) | BIT(4) | BIT(3) | BIT(2));
        if (n != s) { cmd0(0x06); op(0x01, 0, 0, &n, 1, 0, 0); wait_ready(50); }
    } else if (mfg == 0xC8 && ((id >> 8) & 0xFF) == 0x40 && (id & 0xFF) >= 0x16) {   /* GD25Q: SR1 = 0, SR2 = QE */
        uint8_t s = rd8(0x05), s2 = rd8(0x35), z = 0, n2 = s2 & BIT(1);
        if (s) { cmd0(0x06); op(0x01, 0, 0, &z, 1, 0, 0); wait_ready(50); }
        if (s2 != n2) { cmd0(0x06); op(0x31, 0, 0, &n2, 1, 0, 0); wait_ready(50); }
    } else {                                                           /* others: SR1+SR2 = QE only (01h, 2 bytes) */
        uint16_t s = rd8(0x05) | rd8(0x35) << 8, n = s & BIT(9);
        if (n != s) { uint8_t b[2] = { (uint8_t)n, (uint8_t)(n >> 8) }; cmd0(0x06); op(0x01, 0, 0, b, 2, 0, 0); wait_ready(50); }
    }
    cmd0(0x04);
}

static void suspend_init(void);

/* MXIC octal flash (IDF spi_flash_oct_flash_init.c s_flash_init_mxic, STR): the chip powers up in 1-line SPI mode;
 * output driver strength 7 (CR bits 2:0), then WRCR2 (0x72, address 0) = 1 selects OPI STR; from then on every
 * command is 16 bits (command, inverse) on 8 lines. Already in OPI (warm start): RDID answers in OPI. 80 MHz STR needs
 * no timing tuning (IDF). Not run on hardware here (this board has quad flash). */
static void oct_init(void)
{
    oct = true;                                                        /* already OPI? */
    uint8_t id[3] = { 0 };
    op(0x9F, 0, 0, 0, 0, id, 3);
    if (id[0] != 0xC2 || (id[1] & 0xF0) != 0x80) {
        oct = false;                                                   /* SPI mode */
        op(0x9F, 0, 0, 0, 0, id, 3);
        if (id[0] != 0xC2 || (id[1] & 0xF0) != 0x80) return;           /* not an MXIC octal part: quad path */
        uint8_t sr = rd8(0x05), cr = rd8(0x15), b[2] = { sr, (uint8_t)((cr & 0xF8) | 7) };
        cmd0(0x06); op(0x01, 0, 0, b, 2, 0, 0); wait_ready(50);        /* WRSR/WRCR: driver strength */
        uint8_t one = 1;
        cmd0(0x06); op(0x72, 32, 0, &one, 1, 0, 0);                    /* WRCR2: OPI STR */
        oct = true;
        wait_ready(50);
    }
    info.jedec_id = (uint32_t)id[0] << 16 | id[1] << 8 | id[2];
    uint8_t c = id[2]; if (c > 0x30) c -= 0x20;                        /* MXIC octal density codes */
    info.size = c >= 0x10 && c <= 0x1F ? 1u << c : 0;                 /* odd ID: unknown size, no undefined shift */
    info.erase_size[0] = 4096; info.erase_cmd[0] = 0x21;               /* 4-byte address commands only */
    info.erase_size[1] = 65536; info.erase_cmd[1] = 0xDC;
    info.erase_size[2] = 0;
    info.addr4 = true; info.octal = true;
    REG(0x60009004u + 4 * 29) = (REG(0x60009004u + 4 * 29) & ~(3u << 10)) | 3u << 10;   /* CS0 drive 3 (IDF) */
}

const flash_info_t *flash_info(void)
{
    if (info_done) return &info;
    info_done = true;
    if (REG(0x6000703Cu) & BIT(9)) {          /* eFuse FLASH_TYPE: 8 data lines (IDF spi_flash_oct_flash_init.c) */
        oct_init();
        if (oct) return &info;
    }
    xmc_startup();
    info.jedec_id = flash_jedec_id();
    uint8_t c = info.jedec_id & 0xFF;
    if ((info.jedec_id >> 16) == 0xC2 && c > 0x30) c -= 0x20;          /* MXIC octal density codes */
    if (c == 0x20 || c == 0x21) c -= 6;                                /* 512 Mb / 1 Gb codes: 64 / 128 MB */
    info.size = c >= 0x10 && c <= 0x1F ? 1u << c : 0;
    info.erase_size[0] = 4096;  info.erase_cmd[0] = 0x20;              /* JEDEC defaults */
    info.erase_size[1] = 32768; info.erase_cmd[1] = 0x52;
    info.erase_size[2] = 65536; info.erase_cmd[2] = 0xD8;
    uint32_t mfg = info.jedec_id >> 16, did = info.jedec_id & 0xFFFF;
    for (unsigned k = 0; k < sizeof chips / sizeof chips[0]; k++)
        if ((chips[k].mfg == mfg && (did & chips[k].mask) == chips[k].id) || !chips[k].mfg) {
            info.qe_method = chips[k].qe; info.suspend = chips[k].flags & F_SUSPEND; break;
        }
    uint32_t h[4], d[16] = { 0 };
    if (!sfdp_read(0, h, 16) && h[0] == 0x50444653u) {                 /* "SFDP": basic flash parameter table */
        uint32_t len = h[2] >> 24, ptr = h[3] & 0xFFFFFF;
        if (len > 16) len = 16;
        if (len >= 9 && !sfdp_read(ptr, d, len * 4)) {
            info.sfdp = (uint8_t)len;
            if (!(d[1] & BIT(31))) info.size = (d[1] + 1) / 8;         /* density: bits - 1 */
            else if ((d[1] & 0x7FFFFFFF) >= 3 && (d[1] & 0x7FFFFFFF) <= 34) info.size = 1u << ((d[1] & 0x7FFFFFFF) - 3);   /* 2^N bits; chip data: no UB shift */
            int n = 0;
            for (int k = 0; k < 4; k++) {                              /* erase types 1-4: size 2^N, opcode */
                uint32_t w = d[7 + k / 2] >> (16 * (k & 1));
                uint8_t sz = w & 0xFF;
                if (sz >= 12 && sz <= 24) { info.erase_size[n] = 1u << sz; info.erase_cmd[n] = (w >> 8) & 0xFF; n++; }
            }
            for (int k = n; k < 4; k++) info.erase_size[k] = 0;
            if (len >= 13 && !(d[11] & BIT(31))) info.suspend = true;  /* suspend/resume supported */
        }
        if (mfg == 0x20) { uint8_t b = 0; sfdp_read(0x32, &b, 1); if (b & 0x8) info.suspend = true; }   /* XMC-D */
    }
    info.sus_cmd = 0x75; info.res_cmd = 0x7A;
    if (info.sfdp >= 13 && !(d[11] & BIT(31))) { info.sus_cmd = d[12] >> 24; info.res_cmd = (d[12] >> 16) & 0xFF; }
    if (NDF_FLASH_SUSPEND == 0) info.suspend = false;
    if (NDF_FLASH_SUSPEND == 2) info.suspend = true;                   /* forced (chip tested by the user) */
    info.addr4 = info.size > (16u << 20);
    /* an erase/program left suspended (reset or power loss while suspended: the flash keeps its state) blocks every
     * new erase/program: resume it and let it finish. SUS = SR2 bit 7 (not on ISSI/MXIC, where 0x35 means "enter QPI") */
    if (info.qe_method != QE_SR1_B6 && (rd8(0x35) & BIT(7))) { cmd0(info.res_cmd); wait_ready(3000); }
    unlock();
    uint32_t s = qe_read();
    if (!(s & qe_bit()) && NDF_FLASH_QE_SET) { qe_write(s | qe_bit()); s = qe_read(); }
    info.quad = (s & qe_bit()) != 0;
    read_lines = info.quad ? 4 : 2;
    if (info.quad) {                                                   /* WP/HD pads to their SPI function */
        REG(0x60009004 + 4 * 27) = BIT(9) | 3u << 10;                  /* SPIHD: input on, drive 3 */
        REG(0x60009004 + 4 * 28) = BIT(9) | 3u << 10;                  /* SPIWP */
    }
    if (info.suspend) suspend_init();
    return &info;
}

uint32_t flash_size(void) { return flash_info()->size; }
static bool in_flash(uint32_t addr, size_t len) { uint32_t n = flash_size(); return addr <= n && len <= n - addr; }   /* no wrap */
static void detect_lines(void) { if (!read_lines) flash_info(); }

int flash_unique_id(uint8_t id[8])
{
    return op_x(0x4B, 0, 0, 0, 0, id, 8, 1, 32);                       /* 4 dummy bytes */
}

/* ---- reads through the data cache (SPI0 + MMU), the fast path ----------------
 * The flash is mapped once at 0x3C000000 (64 KB MMU pages, first 16 MB). SPI0, the cache's flash controller, is
 * switched from the ROM's DIO to QIO (0xEB, 6 dummy clocks) and the cache to 64-byte lines: 4 KB cold in ~131 us
 * (SPI1 register path: 209 us), repeated reads are cache hits. Writes and erases go through SPI1 and invalidate the
 * cache afterwards. Above 16 MB: SPI1 with 4-byte address commands. */
#define DCACHE_CTRL   0x600C4000      /* b0 enable b3..4 block size (2 = 64 B) */
#define DCACHE_CTRL1  0x600C4004      /* b0/b1 shut dbus core0/core1 */
#define DCACHE_SYNC   0x600C4028      /* b0 invalidate b3 done */
#define MMU(i)        (0x600C5000 + 4 * (i))
#define S0            0x60003000
#define FLASH_VADDR   0x3C000000u
#define MAP_MAX       (16u << 20)
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
    if (addr >= MAP_MAX) return;
    uint32_t a = (FLASH_VADDR + addr) & ~63u;
    REG(DCACHE_SYNC_ADDR) = a;
    REG(DCACHE_SYNC_SIZE) = ((FLASH_VADDR + addr + len + 63) & ~63u) - a;
    dcache_sync();
}

/* ---- 120 MHz (IDF mspi_timing_tuning.c, STR, core clock 120 MHz / divider 1) -------------------------------
 * At 120 MHz the data from the flash arrives too late for the default sampling point: sweep IDF's table of input
 * delay settings {DIN mode, DIN num, extra dummy} (mspi_timing_tuning_configs.h, CORE_CLK_120M_MODULE_CLK_120M_STR),
 * reading 64 reference bytes (flash 0x0, read first at 80 MHz) over SPI1 each time; take the middle of the passing
 * run (IDF: run of 3 or 4). No good run: stay at 80 MHz. SPI0 and SPI1 share the core clock and the DIN registers. */
static int spi1_read(uint32_t addr, void *buf, size_t len);
#define CORE_CLK_SEL  (S0 + 0xEC)     /* b1:0 MSPI core clock: 0 80 MHz, 1 120, 2 160, 3 240 */
#define TIMING_CALI   (S0 + 0xA8)     /* b0 timing clock on */
#define DIN_MODE      (S0 + 0xAC)     /* 3 bits per line (D0-D7, DQS) */
#define DIN_NUM       (S0 + 0xB0)     /* 2 bits per line */
static uint32_t flash_mhz = 80;

static void din_set(uint32_t mode, uint32_t num)
{
    uint32_t m = 0, n = 0;
    for (int k = 0; k < 9; k++) { m |= mode << (3 * k); n |= num << (2 * k); }
    REG(DIN_MODE) = m; REG(DIN_NUM) = n;
}

static void clock_120(void)
{
    static const uint8_t tab[12][3] = { {2, 0, 1}, {0, 0, 0}, {2, 2, 2}, {1, 0, 1}, {2, 0, 2}, {0, 0, 1},
                                        {2, 2, 3}, {1, 0, 2}, {2, 0, 3}, {0, 0, 2}, {2, 2, 4}, {1, 0, 3} };
    uint8_t ref[64], rd[64];
    REG(CORE_CLK_SEL) &= ~3u; din_set(0, 0); xdummy = 0;            /* the warm-boot stage may have left 120 MHz */
    REG(S0 + 0x14) = BIT(31); REG(S1 + 0x14) = BIT(31);              /* 80 MHz reference read */
    if (spi1_read(0, ref, sizeof ref)) return;
    uint32_t c0 = REG(S0 + 0x14), c1 = REG(S1 + 0x14), sel = REG(CORE_CLK_SEL);
    REG(CORE_CLK_SEL) = (sel & ~3u) | 1;                              /* core 120 MHz */
    REG(S0 + 0x14) = BIT(31); REG(S1 + 0x14) = BIT(31);              /* SPI clock = core clock */
    REG_SET(TIMING_CALI, BIT(0));
    uint32_t note = REG(0x50001FE4u);                                 /* window found earlier (us or the loader) */
    if ((note & 0xFFFF0000u) == 0x54310000u) {
        din_set((note >> 8) & 7, (note >> 4) & 3); xdummy = note & 15;
        memset(rd, 0, sizeof rd);
        if (!spi1_read(0, rd, sizeof rd) && !memcmp(rd, ref, sizeof rd)) { flash_mhz = 120; return; }
    }
    int run = 0, best_len = 0, best_end = -1;
    for (int k = 0; k < 12; k++) {
        din_set(tab[k][0], tab[k][1]); xdummy = tab[k][2];
        memset(rd, 0, sizeof rd);
        bool ok = !spi1_read(0, rd, sizeof rd) && !memcmp(rd, ref, sizeof rd);
        run = ok ? run + 1 : 0;
        if (run > best_len) { best_len = run; best_end = k; }
    }
    if (best_len < 3 || best_len > 4) {                              /* no reliable window: back to 80 MHz */
        REG(0x50001FE4u) = 0;
        din_set(0, 0); xdummy = 0;
        REG(CORE_CLK_SEL) = sel; REG(S0 + 0x14) = c0; REG(S1 + 0x14) = c1;
        return;
    }
    int b = best_end - best_len / 2;
    din_set(tab[b][0], tab[b][1]); xdummy = tab[b][2];
    flash_mhz = 120;
    REG(0x50001FE4u) = 0x54310000u | tab[b][0] << 8 | tab[b][1] << 4 | tab[b][2];   /* for the warm-boot stage (loader.c) */
}

static void mm_init(void)
{
    detect_lines();
#if SOC_HAS_FLASH_CACHE
    bool quad = read_lines == 4 || oct;
    if ((quad && NDF_FLASH_READ == NDF_FLASH_READ_AUTO) || NDF_XIP) {   /* code from flash needs the mapping in any mode */
        if (oct) {                             /* cache read: 8READ, 32-bit address, 20 dummy (variable), as IDF */
            REG(S0 + 0x08) = (REG(S0 + 0x08) & ~OCT_MASK) | OCT_CTRL;
            REG(S0 + 0x1C) = 31u << 26 | 19;
            REG(S0 + 0x20) = 15u << 28 | CMD16(0xEC);
            REG_SET(S0 + 0x3C, BIT(1));                                 /* CACHE_FCTRL: 4-byte cache address */
            REG_SET(S0 + 0xE0, BIT(1));                                 /* DDR: variable dummy */
        } else if (quad) {
            REG(S0 + 0x08) = (REG(S0 + 0x08) & ~(BIT(14) | BIT(20) | BIT(23))) | BIT(13) | BIT(24);   /* QIO */
            if (NDF_FLASH_MHZ == 120) clock_120();
            REG(S0 + 0x1C) = (REG(S0 + 0x1C) & ~0x3Fu) | (5 + xdummy);                             /* 6 dummy clocks */
            REG(S0 + 0x20) = 7u << 28 | 0xEB;                                                       /* cache command */
        }                                                                                           /* else ROM's DIO */
        REG_CLR(DCACHE_CTRL, BIT(0));
        REG(DCACHE_CTRL) = (REG(DCACHE_CTRL) & ~(3u << 3)) | 2u << 3;                           /* 64-byte lines */
        uint32_t pages = (flash_size() < MAP_MAX ? flash_size() : MAP_MAX) >> 16;
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
    if (!in_flash(addr, len)) return -1;
    if (mm > 0 && addr + len <= MAP_MAX) {
        memcpy(buf, (const void *)(FLASH_VADDR + addr), len);   /* cache-line bursts, hits on repeats */
        return 0;
    }
    return spi1_read(addr, buf, len);
}

static int spi1_read(uint32_t addr, void *buf, size_t len)
{
    detect_lines();
    /* Quad: QIO read (0xEB, address on 4 lines, 6 dummy clocks); 4-byte address variants (0xEC / 0x3C) above 16 MB.
     * 32-byte transactions alternate between the low (W0-7) and high (W8-15) data buffer halves, so draining one half
     * overlaps the next transfer. */
    uint32_t ctrl = REG(CTRL);
    bool q = read_lines == 4, a4 = info.addr4 && addr + len > (16u << 20);
    REG(CTRL) = oct ? (ctrl & ~OCT_MASK) | OCT_CTRL : (ctrl & ~MODE_MASK) | BIT(13) | (q ? BIT(24) : BIT(14));
    REG(USER1) = oct ? 31u << 26 | 19 : (a4 ? 31u : 23u) << 26 | ((q ? 5 : 7) + xdummy);   /* (+ tuning) */
    REG(USER2) = oct ? CMD16(0xEC) | 15u << 28 : (q ? (a4 ? 0xEC : 0xEB) : (a4 ? 0x3C : 0x3B)) | 7u << 28;
    const uint32_t user = BIT(31) | BIT(30) | BIT(29) | BIT(28) | BIT(6) | BIT(7);
    uint8_t *b = buf;
    int rc = 0;
    size_t n = len > 32 ? 32 : len;
    int half = 0;                                            /* 0: W0-7, 1: W8-15 */
    REG(USER) = user;
    REG(ADDR) = addr;                                        /* plain address; QIO mode bits ride in the dummy clocks */
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

/* ---- erase / program ------------------------------------------------------------
 * One path for every erase and program: a user command with the program/erase flag (CMD b17). With suspend on
 * (suspend_init, the IDF auto-suspend setup: spi_flash_hal_setup_auto_suspend_mode) SPI1 polls the chip itself
 * (auto wait-idle) and a cache miss from either core suspends the operation, reads, and resumes it: code and data in
 * flash keep running during a long erase. Without suspend, flash code is paused meanwhile (cache_flash_op_begin). */
static void suspend_init(void)
{
    REG(SUS_CTRL) = (uint32_t)info.sus_cmd << 9 | (uint32_t)info.res_cmd << 1 | BIT(0);   /* auto suspend */
    REG(WAITI_CTRL) = 0x05u << 2 | BIT(0);                                              /* auto wait-idle, RDSR */
    REG(SUS_CMD) = BIT(4) | BIT(3) | BIT(2);       /* suspend on SPI0 request; wait idle after suspend and resume */
    REG_SET(MISC, BIT(11));                        /* auto resume */
    /* tSUS (IDF: tsus_val x MHz / 256, rounded up; units of 256 SPI clocks at 80 MHz) */
    REG(CTRL1) = (REG(CTRL1) & ~(0x3FFu << 2)) | ((NDF_FLASH_SUS_HOLD_US * 80 + 255) / 256) << 2;
    REG_SET(SUS_STATUS, BIT(5) | BIT(6));
    REG(S0 + 0x10) = (REG(S0 + 0x10) & ~(0x3Fu << 25)) | 3u << 25;   /* SPI0 CS hold: tSHSL2 30 ns at 80 MHz */
    REG(CTRL) = BIT(18) | BIT(19);                 /* SPI1 as IDF leaves it: Q/D polarity only (no ROM read-mode bits) */
    REG(S1 + 0xE8) = 0; REG(S0 + 0xE8) = 0;        /* CLOCK_GATE: automatic, as IDF */
}

static int pe_wait(uint32_t ms)                 /* until the program/erase is done (flash idle, not suspended) */
{
    /* as IDF spi_flash_chip_generic_wait_idle: wait until SPI1 is idle or the operation is suspended, then read the
     * status over SPI1 (that SPI1 transaction is what lets the controller resume a suspended operation), every 20 us */
    uint32_t t = ccount(), lim = ms * (sys_cpu_hz() / 1000);
    for (;;) {
        while (REG(CMD) && !(REG(SUS_STATUS) & BIT(0))) if (ccount() - t > lim) return -2;
        bool sus = REG(SUS_STATUS) & BIT(0);
        if (!(rd8(0x05) & 1) && !sus && !REG(CMD)) return 0;
        if (ccount() - t > lim) return -2;
        delay_us(20);
    }
}

/* WREN, then cmd (+ address, + data) as a program/erase transaction; returns once it is done */
static int pe(uint8_t cmd, bool has_addr, uint32_t addr, const uint8_t *data, size_t n, uint32_t ms)
{
    if (pe_wait(ms)) return -1;
    bool a4 = info.addr4 && addr >= (16u << 20);
    /* the controller's own commands where one exists (SE 0x20, BE 0xD8, CE 0xC7, PP 0x02, 3-byte address): auto
     * wait-idle and auto suspend/resume act on these (IDF uses them: spimem_flash_ll_erase_sector etc.); others
     * (32 KB erase, 4-byte address) as a user command flagged program/erase */
    uint32_t hw = !info.suspend || a4 ? 0 : cmd == 0x20 ? BIT(24) : cmd == 0xD8 ? BIT(23) : cmd == 0xC7 ? BIT(22) :
                  cmd == 0x02 ? BIT(25) : 0;          /* only with hardware auto-suspend (mixing them with user
                                                         commands leaves the next user erase unaccepted: WEL stays) */
    uint32_t ctrl = REG(CTRL);
    if (hw) { REG(CMD) = BIT(30); for (uint32_t t = ccount(); REG(CMD); ) if (ccount() - t > 2400000) return -1; }
    else if (cmd0(0x06)) return -1;                                   /* WREN */
    if (oct) hw = 0;                                                  /* controller commands are 1-line */
    REG(CTRL) = oct ? (ctrl & ~OCT_MASK) | OCT_CTRL : ctrl & ~MODE_MASK;
    REG(USER) = (hw ? 0 : BIT(31) | (has_addr ? BIT(30) : 0) | (n ? BIT(27) : 0)) | BIT(6) | BIT(7);
    REG(USER1) = (has_addr ? (a4 || oct ? 31u : 23u) : 0) << 26;
    REG(USER2) = oct ? CMD16(cmd) | 15u << 28 : cmd | 7u << 28;
    REG(ADDR) = hw == BIT(25) ? addr | n << 24 : addr;               /* page program: length in the top byte */
    REG(MOSI_DLEN) = n ? n * 8 - 1 : 0;
    REG(MISO_DLEN) = 0;
    for (size_t i = 0; i < (n + 3) / 4; i++) {
        uint32_t v = 0;
        for (int b = 0; b < 4 && i * 4 + b < n; b++) v |= (uint32_t)data[i * 4 + b] << (8 * b);
        REG(W(i)) = v;
    }
    REG(CMD) = hw ? hw : BIT(18) | BIT(17);
    int r = pe_wait(ms);
    REG(CTRL) = ctrl;
    return r;
}

static uint8_t cmd4(uint8_t c, uint32_t addr)   /* the 4-byte address variant above 16 MB */
{
    if (!info.addr4 || (addr < (16u << 20) && !oct)) return c;
    return c == 0x20 ? 0x21 : c == 0x52 ? 0x5C : c == 0xD8 ? 0xDC : c == 0x02 ? 0x12 : c;
}

static uint32_t op_begin(void) { return info.suspend ? 0 : cache_flash_op_begin(); }
static void op_end(uint32_t g, uint32_t addr, size_t len)
{
    if (info.suspend) cache_invalidate_code_range(addr, len);
    else cache_flash_op_end(g, addr, len);
    if (mm > 0) dcache_invalidate_range(addr, len);                   /* cached old contents */
}

static int erase_one(int k, uint32_t addr)
{
    uint32_t g = op_begin();
    int r = pe(cmd4(info.erase_cmd[k], addr), true, addr, 0, 0, info.erase_size[k] > 4096 ? 2000 : 500);
    op_end(g, addr, info.erase_size[k]);
    return r;
}

int flash_erase_sector(uint32_t addr)
{
    flash_info();
    addr &= ~(FLASH_SECTOR - 1);
    if (addr >= info.size) return -1;
    return erase_one(0, addr);
}

int flash_erase(uint32_t addr, size_t len)
{
    flash_info();
    if ((addr | len) & (FLASH_SECTOR - 1) || !in_flash(addr, len)) return -1;
    while (len) {
        int k = 3;                                                     /* largest erase type that fits here */
        while (k > 0 && (!info.erase_size[k] || (addr & (info.erase_size[k] - 1)) || len < info.erase_size[k])) k--;
        if (erase_one(k, addr)) return -1;
        addr += info.erase_size[k]; len -= info.erase_size[k];
    }
    return 0;
}

int flash_erase_chip(void)
{
    flash_info();
    uint32_t g = cache_flash_op_begin();                               /* chip erase is not suspendable */
    if (info.suspend) REG_CLR(SUS_CTRL, BIT(0));
    int r = pe(0xC7, false, 0, 0, 0, 400000);                          /* datasheets: up to ~200 s for 16 MB */
    if (info.suspend) REG_SET(SUS_CTRL, BIT(0));
    cache_flash_op_end(g, 0, 0);
    cache_invalidate_code();
    if (mm > 0) dcache_invalidate();
    return r;
}

int flash_write(uint32_t addr, const void *buf, size_t len)
{
    flash_info();
    if (!in_flash(addr, len)) return -1;
    uint32_t a0 = addr; size_t l0 = len;
    const uint8_t *b = buf;
    uint32_t g = op_begin();
    while (len) {
        /* stay inside one 256-byte page and one 64-byte transaction */
        size_t n = FLASH_PAGE - (addr & (FLASH_PAGE - 1));
        if (n > 64) n = 64;
        if (n > len) n = len;
        if (pe(cmd4(0x02, addr), true, addr, b, n, 10)) { op_end(g, a0, l0); return -1; }
        addr += n; b += n; len -= n;
        if (!info.suspend && len && !(addr & (FLASH_PAGE - 1))) {   /* between pages the chip is idle: let flash code */
            cache_flash_op_end(g, addr, 0);                          /* and interrupts run (wait <= one page program) */
            g = cache_flash_op_begin();
        }
    }
    op_end(g, a0, l0);
    return 0;
}
int flash_read_lines(void) { return read_lines; }
uint32_t flash_clock_mhz(void) { return flash_mhz; }
void flash_clock_80(void)              /* back to the 80 MHz sampling (PSRAM needs the core clock at 160 MHz) */
{
    din_set(0, 0); xdummy = 0; flash_mhz = 80;
    if ((REG(S0 + 0x20) & 0xFFFF) == 0xEB) REG(S0 + 0x1C) = (REG(S0 + 0x1C) & ~0x3Fu) | 5;   /* QIO: 6 dummy clocks */
}
bool flash_cached(void) { if (!mm) mm_init(); return mm > 0; }
bool flash_cache_map(void) { return flash_cached(); }
