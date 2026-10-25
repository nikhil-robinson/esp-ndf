/* Octal PSRAM (ESP32-S3R8: 8 MB AP Memory, in package, on MSPI CS1), esp-ndf's own driver.
 * Register sequence from the TRM (SPI_MEM, IO MUX, cache MMU) and the vendor's octal driver as a reference
 * (esp_psram_impl_octal.c): pins, CS timing, mode registers over SPI1 in octal DTR, then SPI0's cache path. */
#include "psram.h"
#include "irq.h"
#include "flash.h"
#include "xtensa.h"
#include "sys.h"
#include <string.h>

#define S0 0x60003000u
#define S1 0x60002000u
#define SR(b, o) (*(volatile uint32_t *)((b) + (o)))
#define IOMUX(g) (*(volatile uint32_t *)(0x60009004u + 4u * (g)))

int psram_bench_clk;
static void pins(void)
{
    for (int g = 27; g <= 32; g++)                                                       /* flash pads shared with PSRAM D0..D3 / CLK:
                                                                                          * MSPI function 0, not the GPIO matrix (else the PSRAM never answers) */
        IOMUX(g) = g == 29 ? 0x0700 : g == 30 ? 0x070A : 0x0F0A;
    IOMUX(26) = (IOMUX(26) & ~(7u << 12 | 3u << 10)) | 0u << 12 | 3u << 10;              /* SPICS1, drive 3 */
    for (int g = 33; g <= 37; g++) IOMUX(g) = 0x4F0A;                                   /* SPIIO4..7, SPIDQS: func 4, drive 3, IE, pull-up */
    SR(S0, 0x3FC) = (SR(S0, 0x3FC) & ~3u) | 3u;                                          /* PSRAM clock drive 3 */
}

/* one octal DTR command on SPI1 to CS1: 16-bit command, 32-bit address, dummy (half-cycles as the PSRAM counts),
 * optional data out / in (bytes, even) */
int psram_cmd(uint16_t cmd, uint32_t addr, int dummy, const uint8_t *out, int nout, uint8_t *in, int nin)
{
    uint32_t misc = SR(S1, 0x34), ctrl = SR(S1, 0x08), ddr = SR(S1, 0xE0), clk = SR(S1, 0x14), fctrl = SR(S1, 0x3C);
    SR(S1, 0x3C) = fctrl | BIT(1);                               /* usr_cmd_4byte: 32-bit address */
    SR(S1, 0x34) = (misc & ~3u) | 1u;                            /* CS0 off, CS1 on */
    SR(S1, 0x08) = (ctrl & ~(BIT(13) | BIT(14) | BIT(20) | BIT(23) | BIT(24))) | BIT(13) | BIT(9) | BIT(6) | BIT(5) | BIT(4);   /* fast read + oct cmd/addr/din/dout (HAL OPI_DTR) */
    SR(S1, 0xE0) = (ddr & ~0x1Fu) | BIT(0) | BIT(1);             /* DDR, variable dummy */
    SR(S1, 0x14) = psram_bench_clk ? (uint32_t)psram_bench_clk : (3u << 16 | 1u << 8 | 3u);   /* slow for set-up */
    SR(S1, 0x18) = BIT(31) | BIT(30) | (dummy ? BIT(29) : 0) | (nout ? BIT(27) : 0) | (nin ? BIT(28) : 0) | BIT(6) | BIT(7);
    SR(S1, 0x1C) = 31u << 26 | (dummy ? (uint32_t)(dummy - 1) : 0);
    SR(S1, 0x20) = 15u << 28 | cmd;
    SR(S1, 0x04) = addr;
    SR(S1, 0x24) = nout ? (uint32_t)nout * 8 - 1 : 0;
    SR(S1, 0x28) = nin ? (uint32_t)nin * 8 - 1 : 0;
    for (int i = 0; i < (nout + 3) / 4; i++) { uint32_t v = 0; for (int b = 0; b < 4 && i * 4 + b < nout; b++) v |= (uint32_t)out[i * 4 + b] << (8 * b); SR(S1, 0x58 + 4 * i) = v; }
    SR(S1, 0x00) = BIT(18);
    uint32_t t = ccount(); int rc = 0;
    while (SR(S1, 0x00) & BIT(18)) if (ccount() - t > 2400000) { rc = -1; break; }
    for (int i = 0; i < nin; i++) in[i] = (uint8_t)(SR(S1, 0x58 + 4 * (i / 4)) >> (8 * (i & 3)));
    SR(S1, 0x34) = misc; SR(S1, 0x08) = ctrl; SR(S1, 0xE0) = ddr; SR(S1, 0x14) = clk; SR(S1, 0x3C) = fctrl;
    return rc;
}

void psram_pins(void) { pins(); }
int psram_probe(uint8_t mr[8])
{
    pins();
    for (int a = 0; a < 8; a += 2) if (psram_cmd(0x4040, (uint32_t)a, 8, 0, 0, mr + a, 2)) return -1;   /* register read, dummy 2*(5-1) */
    return 0;
}

/* SPI0 (cache) path to CS1, as the vendor's s_config_psram_spi_phases: octal DTR, 16-bit commands
 * (read 0x0000, write 0x8080), 32-bit address, read dummy rd, write dummy wr (cycles), variable dummy. */
void psram_spi0_setup(int clk_div, int rd_dummy, int wr_dummy)
{
    SR(S0, 0xDC) = (SR(S0, 0xDC) & ~(0x1Fu << 7 | 0x1Fu << 2 | 0x1Fu << 25)) | BIT(0) | BIT(1) | 3u << 7 | 3u << 2 | 2u << 25;   /* CS setup / hold */
    uint32_t sc = SR(S0, 0x40);
    sc &= ~(0x3Fu << 22 | 0x3Fu << 14 | 0x3Fu << 6);
    sc |= BIT(20) | BIT(5) | 31u << 14 | BIT(0) | BIT(4) | BIT(3) | (uint32_t)(rd_dummy - 1) << 6 | (uint32_t)(wr_dummy - 1) << 22 | BIT(21);
    SR(S0, 0x40) = sc;                                                   /* wr/rd cmd, 4-byte addr, dummies, octal */
    SR(S0, 0x4C) = 15u << 28 | 0x8080;                                   /* write command */
    SR(S0, 0x48) = 15u << 28 | 0x0000;                                   /* read command */
    SR(S0, 0xE4) = (SR(S0, 0xE4) & ~(BIT(2) | BIT(3))) | BIT(0) | BIT(1);   /* DDR, variable dummy, no swap */
    SR(S0, 0x44) |= BIT(22) | BIT(21) | BIT(20) | BIT(19) | BIT(18);     /* octal dummy-out / cmd / addr / dout / din */
    uint32_t n = (uint32_t)clk_div - 1;
    SR(S0, 0x50) = n << 16 | ((n + 1) / 2 - 1) << 8 | n;                 /* SRAM clock: core clock / clk_div */
}

/* bench: the same SPI1 path, 1-bit, CS0 (flash): JEDEC ID 0x9F, to validate the command sequence */
uint32_t psram_bench_jedec(void)
{
    uint32_t misc = SR(S1, 0x34), ctrl = SR(S1, 0x08);
    SR(S1, 0x34) = (misc & ~3u) | 2u;                            /* CS0 on, CS1 off */
    SR(S1, 0x08) = ctrl & ~(BIT(13) | BIT(14) | BIT(20) | BIT(23) | BIT(24) | BIT(9) | BIT(6) | BIT(5) | BIT(4));
    SR(S1, 0x18) = BIT(31) | BIT(28) | BIT(6) | BIT(7);
    SR(S1, 0x1C) = 0; SR(S1, 0x20) = 7u << 28 | 0x9F; SR(S1, 0x28) = 23; SR(S1, 0x58) = 0;
    SR(S1, 0x00) = BIT(18);
    uint32_t t = ccount(); while ((SR(S1, 0x00) & BIT(18)) && ccount() - t < 2400000) {}
    uint32_t v = SR(S1, 0x58);
    SR(S1, 0x34) = misc; SR(S1, 0x08) = ctrl;
    return v;
}

#define DCACHE_SYNC 0x600C4028u
/* Erratum CACHE-126 (all revisions v0.0-v0.2): a write-back can return wrong data to a concurrent cache hit. As the
 * errata workaround: interrupts off on this core and the D-cache frozen (EXTMEM_DCACHE_FREEZE, mode 0: the other
 * core stalls on a miss) for the duration of the write-back. */
#define DCACHE_FREEZE 0x600C4150u                          /* b0 ena b1 mode b2 done */
static void dcache_op(uint32_t addr, uint32_t size, uint32_t op)   /* op: 1 invalidate, 4 write back (clean) */
{
    uint32_t ps = irq_lock();
    bool wb = op == 4;
    if (wb) {
        *(volatile uint32_t *)DCACHE_FREEZE = BIT(0);
        for (uint32_t t = ccount(); !(*(volatile uint32_t *)DCACHE_FREEZE & BIT(2)) && ccount() - t < 2400000; ) {}
    }
    *(volatile uint32_t *)0x600C402C = addr; *(volatile uint32_t *)0x600C4030 = size; *(volatile uint32_t *)DCACHE_SYNC = op;
    uint32_t t = ccount(); while (!(*(volatile uint32_t *)DCACHE_SYNC & 8) && ccount() - t < 24000000) {}
    if (wb) *(volatile uint32_t *)DCACHE_FREEZE = 0;
    irq_unlock(ps);
}
void psram_writeback(void *p, size_t n) { uint32_t a = (uint32_t)p & ~63u; dcache_op(a, (((uint32_t)p + n + 63) & ~63u) - a, 4); }
void psram_invalidate(void *p, size_t n) { uint32_t a = (uint32_t)p & ~63u; dcache_op(a, (((uint32_t)p + n + 63) & ~63u) - a, 1); }

static uint32_t size_bytes;
int psram_line_bytes = 64;                    /* 64 B: 49.5 / 84.6 MB/s write / read; 32 B (IDF): 35.6 / 58.2; 16 B (ROM default): 22.7 / 36.6 */
uint32_t psram_size(void) { return size_bytes; }

/* bring-up: pins, MSPI clocks/timing (as IDF leaves them), PSRAM MR0 (fixed latency, read latency code 2, as IDF),
 * SPI0 cache path, all pages mapped at PSRAM_VADDR. clk_div: SRAM clock = 160 MHz / clk_div. Returns bytes or 0. */
uint32_t psram_init(int clk_div)
{
    uint8_t mr[4];
    pins();
    flash_clock_80();                                            /* flash and PSRAM share the core clock (IDF: flash 80 MHz) */
    SR(S0, 0xEC) = 2;                                            /* MSPI core clock 160 MHz */
    SR(S0, 0x14) = 1u << 16 | 0u << 8 | 1u;                      /* flash SPI0: 160 / 2 = 80 MHz (SPI1 likewise) */
    *(volatile uint32_t *)0x60002014u = 1u << 16 | 0u << 8 | 1u;
    SR(S0, 0xBC) = 7; SR(S0, 0xC0) = 0x01249249;                 /* input timing (IDF values) */
    SR(S0, 0x3FC) = (SR(S0, 0x3FC) & ~0x3Fu) | 0x1F;
    /* mode-register reads use the read latency MR0 holds, and the PSRAM keeps MR0 through chip resets (an earlier
     * app may have set another latency): try the dummy counts of every latency until the vendor ID reads back */
    int dm = -1;
    for (int d = 8; d <= 40 && dm < 0; d += 2) {
        if (psram_cmd(0x4040, 0, d, 0, 0, mr, 2)) continue;
        if ((mr[1] & 0x1F) == 0x0D || (mr[1] & 0x1F) == 0x1A) dm = d;                   /* AP / UnilC */
    }
    if (dm < 0 || psram_cmd(0x4040, 2, dm, 0, 0, mr + 2, 2)) return 0;
    static const uint32_t sizes[8] = { 0, 4u << 20, 0, 8u << 20, 0, 16u << 20, 64u << 20, 32u << 20 };
    size_bytes = sizes[mr[2] & 7];
    uint8_t w[2] = { (uint8_t)((mr[0] & 0xC0) | 1u << 5 | 2u << 2 | 0), mr[1] };          /* MR0: fixed latency, RL code 2, drive full */
    psram_cmd(0xC0C0, 0, 0, w, 2, 0, 0);
    psram_spi0_setup(clk_div, 18, 8);
    /* data cache lines: 16 B by default after ROM boot; 32 B as IDF, or 64 B (flash driver) burst far better */
    if (psram_line_bytes) {
        uint32_t bs = psram_line_bytes >= 64 ? 2 : psram_line_bytes >= 32 ? 1 : 0;
        uint32_t c = *(volatile uint32_t *)0x600C4000;
        if (((c >> 3) & 3) != bs) {
            *(volatile uint32_t *)0x600C4000 = c & ~1u;
            *(volatile uint32_t *)0x600C4000 = (c & ~(1u | 3u << 3)) | bs << 3;
            dcache_op(0, 0, 1);
            *(volatile uint32_t *)0x600C4000 |= 1u;
        }
    }
    /* the 512-entry MMU table is shared by the data and instruction windows: 16 MB at entry 256 reaches the entries of
     * the XIP code window (0x43F00000 = entry 496). Map only free entries (bit 14 = invalid); stop at the first one in
     * use and report the mapped size (BOX-3 with XIP: 15 MB). */
    uint32_t pages = size_bytes >> 16, i = 0;
    for (; i < pages; i++) {
        volatile uint32_t *e = (volatile uint32_t *)(0x600C5000u + 4u * (PSRAM_MMU_FIRST + i));
        if (!(*e & BIT(14)) && !(*e & BIT(15))) break;           /* taken by flash (XIP) */
        *e = 1u << 15 | i;                                      /* SPIRAM page i */
    }
    dcache_op(0, 0, 1);
    size_bytes = i << 16;
    return size_bytes;
}
