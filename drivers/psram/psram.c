/* Octal PSRAM (ESP32-S3R8: 8 MB AP Memory, in package, on MSPI CS1), esp-ndf's own driver.
 * Register sequence from the TRM (SPI_MEM, IO MUX, cache MMU) and the vendor's octal driver as a reference
 * (esp_psram_impl_octal.c): pins, CS timing, mode registers over SPI1 in octal DTR, then SPI0's cache path. */
#include "psram.h"
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
static void dcache_op(uint32_t addr, uint32_t size, uint32_t op)   /* op: 1 invalidate, 4 write back (clean) */
{
    *(volatile uint32_t *)0x600C402C = addr; *(volatile uint32_t *)0x600C4030 = size; *(volatile uint32_t *)DCACHE_SYNC = op;
    uint32_t t = ccount(); while (!(*(volatile uint32_t *)DCACHE_SYNC & 8) && ccount() - t < 24000000) {}
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
    SR(S0, 0xEC) = 2;                                            /* MSPI core clock 160 MHz */
    SR(S0, 0xBC) = 7; SR(S0, 0xC0) = 0x01249249;                 /* input timing (IDF values) */
    SR(S0, 0x3FC) = (SR(S0, 0x3FC) & ~0x3Fu) | 0x1F;
    if (psram_cmd(0x4040, 0, 8, 0, 0, mr, 2) || psram_cmd(0x4040, 2, 8, 0, 0, mr + 2, 2)) return 0;
    if ((mr[1] & 0x1F) != 0x0D && (mr[1] & 0x1F) != 0x1A) return 0;                    /* AP / UnilC */
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
    uint32_t pages = size_bytes >> 16;
    for (uint32_t i = 0; i < pages; i++) *(volatile uint32_t *)(0x600C5000u + 4u * (PSRAM_MMU_FIRST + i)) = 1u << 15 | i;   /* SPIRAM page i */
    dcache_op(0, 0, 1);
    return size_bytes;
}
