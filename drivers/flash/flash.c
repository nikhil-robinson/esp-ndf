/* SPI NOR via SPI1 user commands (TRM ch. 30, SPI_MEM). 1-bit transfers,
 * up to 64 data bytes per transaction through W0..W15. */
#include "flash.h"
#include "xtensa.h"
#include "sys.h"

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

int flash_read(uint32_t addr, void *buf, size_t len)
{
    if (!read_lines) {                          /* quad only if the flash has QE set */
        uint8_t sr2 = 0;
        op(0x35, 0, 0, 0, 0, &sr2, 1);
        read_lines = (sr2 & 0x02) ? 4 : 2;
        if (read_lines == 4) {                   /* WP/HD pads to their SPI function (IO_MUX func 0) */
            REG(0x60009004 + 4 * 27) = BIT(9) | 3u << 10;   /* SPIHD: input on, drive 3 */
            REG(0x60009004 + 4 * 28) = BIT(9) | 3u << 10;   /* SPIWP */
        }
    }
    /* configure once, then per 64-byte chunk only address + start + copy out */
    uint32_t ctrl = REG(CTRL);
    REG(CTRL) = (ctrl & ~MODE_MASK) | BIT(13) | (read_lines == 4 ? BIT(20) : BIT(14));
    REG(USER) = BIT(31) | BIT(30) | BIT(29) | BIT(28) | BIT(6) | BIT(7);
    REG(USER1) = 23u << 26 | 7;                              /* 24-bit address, 8 dummy clocks */
    REG(USER2) = (read_lines == 4 ? 0x6B : 0x3B) | 7u << 28;
    uint8_t *b = buf;
    int rc = 0;
    while (len) {
        size_t n = len > 64 ? 64 : len;
        REG(ADDR) = addr;
        REG(MISO_DLEN) = n * 8 - 1;
        REG(CMD) = BIT(18);
        uint32_t t = ccount();
        while (REG(CMD) & BIT(18)) if (ccount() - t > 2400000) { rc = -1; goto out; }
        if (!((uintptr_t)b & 3) && n == 64) {
            for (int i = 0; i < 16; i++) ((uint32_t *)b)[i] = REG(W(i));
        } else {
            for (size_t i = 0; i < n; i++) b[i] = REG(W(i / 4)) >> (8 * (i & 3));
        }
        addr += n; b += n; len -= n;
    }
out:
    REG(CTRL) = ctrl;
    return rc;
}

int flash_erase_sector(uint32_t addr)
{
    if (addr >= 16u << 20) return -1;
    if (wait_ready(10) || op(0x06, 0, 0, 0, 0, 0, 0) || op(0x20, 24, addr & ~(FLASH_SECTOR - 1), 0, 0, 0, 0)) return -1;
    return wait_ready(500);                                   /* datasheet max ~400 ms */
}

int flash_write(uint32_t addr, const void *buf, size_t len)
{
    if (addr + len > 16u << 20) return -1;
    const uint8_t *b = buf;
    while (len) {
        /* stay inside one 256-byte page and one 64-byte transaction */
        size_t n = FLASH_PAGE - (addr & (FLASH_PAGE - 1));
        if (n > 64) n = 64;
        if (n > len) n = len;
        if (op(0x06, 0, 0, 0, 0, 0, 0) || op(0x02, 24, addr, b, n, 0, 0) || wait_ready(10)) return -1;
        addr += n; b += n; len -= n;
    }
    return 0;
}
int flash_read_lines(void) { return read_lines; }
