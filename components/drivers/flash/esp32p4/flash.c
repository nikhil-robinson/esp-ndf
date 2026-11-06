/* P4 SPI NOR flash through SPI1, same API as drivers/flash/flash.c (flash.h). SPI1 (0x5008D000, ESP-IDF v6.1
 * spi1_mem_c_struct.h) has the S3's command engine bit for bit: CMD.usr starts a user command, USER / USER1 / USER2
 * set cmd / addr / dummy / data phases, up to 64 bytes in the data buffer at +0x58.
 * Plain 1-bit commands (read 0x0B), page program 0x02, erase 0x20 / 0xD8 / 0xC7, 4-byte address commands above 16 MB.
 * ponytail: no SFDP / chip table / quad reads / auto-suspend / XIP yet (flash_cached() is false, so code runs from
 * L2 RAM); the clock is whatever the ROM left in SPI1. Quad reads and the P4 cache + MMU window are the next step. */
#include "flash.h"
#include "soc.h"
#include "cpu.h"
#include "sys.h"
#include "cache_p4.h"

#define S1         FLASH_SPI1_BASE
#define CMD        (S1 + 0x00)   /* b18 usr start */
#define ADDR       (S1 + 0x04)
#define CTRL       (S1 + 0x08)   /* dual / quad read mode bits: cleared for 1-bit commands */
#define USER       (S1 + 0x18)   /* b31 cmd b30 addr b29 dummy b28 miso b27 mosi */
#define USER1      (S1 + 0x1C)   /* b31:26 addr bitlen-1, b5:0 dummy clocks-1 */
#define USER2      (S1 + 0x20)   /* b15:0 cmd value b31:28 cmd bitlen-1 */
#define MOSI_DLEN  (S1 + 0x24)
#define MISO_DLEN  (S1 + 0x28)
#define W(i)       (S1 + 0x58 + 4 * (i))
#define MODE_MASK  (BIT(13) | BIT(14) | BIT(20) | BIT(23) | BIT(24))   /* fastrd, dual, quad, dio, qio */

static flash_info_t info;
static bool info_done;

static int op(uint8_t cmd, int addr_bits, uint32_t addr, const uint8_t *out, size_t nout, uint8_t *in, size_t nin, int dummy)
{
    if (nout > 64 || nin > 64) return -1;
    uint32_t ctrl = REG(CTRL);
    REG(CTRL) = ctrl & ~MODE_MASK;
    REG(USER) = BIT(31) | (addr_bits ? BIT(30) : 0) | (nout ? BIT(27) : 0) | (nin ? BIT(28) : 0) | (dummy ? BIT(29) : 0) | BIT(6) | BIT(7);
    REG(USER1) = (addr_bits ? (uint32_t)(addr_bits - 1) : 0) << 26 | (dummy ? (uint32_t)(dummy - 1) : 0);
    REG(USER2) = cmd | 7u << 28;
    REG(ADDR) = addr;
    REG(MOSI_DLEN) = nout ? nout * 8 - 1 : 0;
    REG(MISO_DLEN) = nin ? nin * 8 - 1 : 0;
    for (size_t i = 0; i < (nout + 3) / 4; i++) {
        uint32_t v = 0;
        for (int b = 0; b < 4 && i * 4 + b < nout; b++) v |= (uint32_t)out[i * 4 + b] << (8 * b);
        REG(W(i)) = v;
    }
    REG(CMD) = BIT(18);
    uint32_t t = ccount(), lim = sys_cpu_hz() / 10;
    while (REG(CMD) & BIT(18)) if (ccount() - t > lim) { REG(CTRL) = ctrl; return -1; }
    for (size_t i = 0; i < nin; i++) in[i] = (uint8_t)(REG(W(i / 4)) >> (8 * (i & 3)));
    REG(CTRL) = ctrl;
    return 0;
}

static uint8_t rd8(uint8_t cmd) { uint8_t v = 0; op(cmd, 0, 0, 0, 0, &v, 1, 0); return v; }

static int wait_ready(uint32_t ms)
{
    uint32_t t = ccount(), lim = ms * (sys_cpu_hz() / 1000);
    do {
        if (!(rd8(0x05) & 1)) return 0;
    } while (ccount() - t < lim);
    return -2;
}

uint32_t flash_jedec_id(void)
{
    uint8_t id[3] = { 0 };
    op(0x9F, 0, 0, 0, 0, id, 3, 0);
    return id[0] << 16 | id[1] << 8 | id[2];
}

const flash_info_t *flash_info(void)
{
    if (!info_done) {
        uint32_t id = flash_jedec_id();
        info.jedec_id = id;
        info.size = (id & 0xFF) >= 0x10 && (id & 0xFF) <= 0x1F ? 1u << (id & 0xFF) : 0;   /* capacity byte = log2(bytes) */
        info.erase_cmd[0] = 0x20; info.erase_size[0] = 4096;
        info.erase_cmd[1] = 0xD8; info.erase_size[1] = 65536;
        info.addr4 = info.size > (16u << 20);
        info_done = true;
    }
    return &info;
}

uint32_t flash_size(void) { return flash_info()->size; }
static bool in_flash(uint32_t addr, size_t len) { uint32_t n = flash_size(); return addr <= n && len <= n - addr; }

int flash_unique_id(uint8_t id[8]) { return op(0x4B, 0, 0, 0, 0, id, 8, 32); }   /* 4 dummy bytes */

static uint8_t cmd4(uint8_t c, uint32_t addr)       /* the 4-byte address variant above 16 MB */
{
    if (!info.addr4 || addr < (16u << 20)) return c;
    return c == 0x20 ? 0x21 : c == 0xD8 ? 0xDC : c == 0x02 ? 0x12 : c == 0x0B ? 0x0C : c;
}
static int abits(uint8_t c) { return c == 0x21 || c == 0xDC || c == 0x12 || c == 0x0C ? 32 : 24; }

int flash_read_raw(uint32_t addr, void *buf, size_t len)   /* 1-bit 0x03 reads, no driver state (panic path, < 16 MB) */
{
    uint8_t *b = buf;
    while (len) {
        size_t n = len > 64 ? 64 : len;
        if (op(0x03, 24, addr, 0, 0, b, n, 0)) return -1;
        addr += n; b += n; len -= n;
    }
    return 0;
}

int flash_read(uint32_t addr, void *buf, size_t len)
{
    flash_info();
    if (!in_flash(addr, len)) return -1;
    if (!cache_flash_read(addr, buf, len)) return 0;                 /* through the cache window once it runs; SPI1 commands before */
    uint8_t *b = buf;
    while (len) {
        size_t n = len > 64 ? 64 : len;
        uint8_t c = cmd4(0x0B, addr);
        if (op(c, abits(c), addr, 0, 0, b, n, 8)) return -1;
        addr += n; b += n; len -= n;
    }
    return 0;
}

static int pe(uint8_t cmd, bool has_addr, uint32_t addr, const uint8_t *data, size_t n, uint32_t ms)
{
    if (op(0x06, 0, 0, 0, 0, 0, 0, 0)) return -1;                       /* write enable */
    if (op(cmd, has_addr ? abits(cmd) : 0, addr, data, n, 0, 0, 0)) return -1;
    return wait_ready(ms);
}

int flash_erase_sector(uint32_t addr)
{
    flash_info();
    addr &= ~(FLASH_SECTOR - 1);
    if (addr >= info.size) return -1;
    return pe(cmd4(0x20, addr), true, addr, 0, 0, 500);
}

int flash_erase(uint32_t addr, size_t len)
{
    flash_info();
    if ((addr | len) & (FLASH_SECTOR - 1) || !in_flash(addr, len)) return -1;
    while (len) {
        if (!(addr & 0xFFFF) && len >= 65536) {
            if (pe(cmd4(0xD8, addr), true, addr, 0, 0, 2000)) return -1;
            addr += 65536; len -= 65536;
        } else {
            if (pe(cmd4(0x20, addr), true, addr, 0, 0, 500)) return -1;
            addr += 4096; len -= 4096;
        }
    }
    return 0;
}

int flash_erase_chip(void)
{
    flash_info();
    return pe(0xC7, false, 0, 0, 0, 400000);                             /* datasheets: up to ~200 s for 16 MB */
}

int flash_write(uint32_t addr, const void *buf, size_t len)
{
    flash_info();
    if (!in_flash(addr, len)) return -1;
    const uint8_t *b = buf;
    while (len) {
        size_t n = FLASH_PAGE - (addr & (FLASH_PAGE - 1));               /* inside one page and one 64-byte transaction */
        if (n > 64) n = 64;
        if (n > len) n = len;
        if (pe(cmd4(0x02, addr), true, addr, b, n, 10)) return -1;
        addr += n; b += n; len -= n;
    }
    return 0;
}

int flash_read_lines(void) { return 1; }
uint32_t flash_clock_mhz(void) { return 0; }
void flash_clock_80(void) {}
bool flash_cached(void) { return false; }
bool flash_cache_map(void) { return false; }
