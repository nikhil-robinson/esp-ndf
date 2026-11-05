/* P4 HEX PSRAM (AP Memory APS256XXN / APS128XXN class, 16 or 32 MB on MSPI2 CS1), same API as drivers/psram/psram.c.
 * Sequence and constants from ESP-IDF v6.1 esp_psram/device/esp_psram_impl_ap_hex.c, esp_hal_mspi psram_ctrlr_ll.h /
 * mspi_ll.h, esp_hal_clock clk_tree_ll.h (MPLL v3), hal ldo_ll.h (PSRAM supply) and mmu_ll.h. Registers are the
 * hw_ver3 spi_mem_s_reg.h block (PSRAM MSPI0 = the AXI/cache path, MSPI1 = user commands).
 * Mode registers are written with the ROM's esp_rom_spi_* command engine (as IDF does), at 80 MHz (MPLL 320 / 4, read
 * latency 2, no timing tuning needed). Higher clocks (200 / 250 MHz) need the DQS / delay-line tuning of
 * mspi_timing_psram_tuning(): not written; psram_init() clamps to 80 MHz.
 * psram_init(clk_div): the SRAM clock is the MPLL divided by clk_div (MPLL 320 MHz, so 4 = 80 MHz, 8 = 40 MHz). */
#include "psram.h"
#include "regi2c.h"
#include "cache_p4.h"
#include "cpu.h"
#include "irq.h"
#include "soc.h"

#define MSPI0            (HPPERIPH0 + 0x8E000)      /* PSRAM MSPI0 (AXI path) */
#define MSPI1            (HPPERIPH0 + 0x8F000)      /* PSRAM MSPI1 (user commands) */
#define FLASH_MSPI0      (HPPERIPH0 + 0x8C000)
#define IOMUX_MSPI       (HPPERIPH1 + 0x21200)
#define M0(o)            (MSPI0 + (o))
#define M1(o)            (MSPI1 + (o))

#define SCTRL            M0(0x40)    /* CACHE_SCTRL: b0 4-byte addr b3 wr dummy en b4 rd dummy en b5 usr rcmd b20 usr wcmd b21 sram_oct b13:6 / b27:22 dummy len b19:14 addr bitlen */
#define SRAM_CMD         M0(0x44)    /* b18 din oct b19 dout oct b20 addr oct b21 cmd oct b23 dummy_wout b26 din hex b27 dout hex */
#define SRAM_DRD         M0(0x48)    /* b15:0 cmd b31:28 bitlen - 1 */
#define SRAM_DWR         M0(0x4C)
#define SRAM_CLK         M0(0x50)
#define CTRL1            M0(0x0C)    /* b25 ar_splice b26 aw_splice */
#define FCTRL            M0(0x3C)    /* b0 axi_req_en b31 close_axi_inf */
#define SMEM_DDR         M0(0xD8)    /* b0 ddr b1 var dummy b2 rd swap b3 wr swap */
#define SMEM_ECC         M0(0x174)   /* b19:18 page size */
#define SMEM_AC          M0(0x1A0)   /* b0 cs_setup b1 cs_hold b6:2 setup time b11:7 hold time b30:25 hold delay b31 split trans */
#define SMEM_CALI        M0(0x190)   /* b5 dll_timing_cali */
#define CALI1            M0(0x180)   /* MSPI1 side (flash/user) timing cali: b5 */
#define MMU_CONTENT      M0(0x37C)
#define MMU_INDEX        M0(0x380)
#define FL_MMU_CONTENT   (FLASH_MSPI0 + 0x37C)
#define FL_MMU_INDEX     (FLASH_MSPI0 + 0x380)

#define SOC_CLK_CTRL0    (HP_CLKRST_BASE + 0x14)    /* b31 psram_sys_clk_en */
#define PERI_CLK_CTRL00  (HP_CLKRST_BASE + 0x30)    /* b14 pll_clk_en b15 core_clk_en b13:12 src (1 = MPLL) b23:16 core div - 1 */
#define HP_RST_EN0       (HP_CLKRST_BASE + 0xC0)    /* b23 dual_mspi_axi b25 dual_mspi_apb */
#define ANA_PLL_CTRL0    (HP_CLKRST_BASE + 0xBC)    /* b8 mspi cal end b9 mspi cal stop */
#define PMU_RF_PWC       (PMU_BASE + 0x15C)         /* b24 MSPI_PHY_XPD */
#define LP_HP_CLK_CTRL   (LP_CLKRST_BASE + 0x40)    /* b28 MPLL 500M clock enable */
#define I2C_MPLL         0x63

#define PSRAM_MMU_VALID  BIT(11)
#define PSRAM_MMU_ACCESS BIT(10)
#define MMU_ENTRIES      1024u

#define MPLL_MHZ         320u
#define CMD_SYNC_RD      0x0000
#define CMD_SYNC_WR      0x8080
#define CMD_REG_RD       0x4040
#define CMD_REG_WR       0xC0C0
#define RD_DUMMY         (2 * (10 - 1))             /* 80 MHz row of the IDF table, bit times */
#define RD_REG_DUMMY     (2 * (5 - 1))
#define WR_DUMMY         (2 * (5 - 1))
#define RD_LATENCY       2u
#define WR_LATENCY       2u
#define REF_DATA         0x5A6B7C8Du

/* ROM command engine (esp32p4.rom.ld esp_rom_spi_cmd_config / _start / _set_op_mode, rom/opi_flash.h) */
typedef struct {
    uint16_t cmd, cmd_bits;
    uint32_t *addr;
    uint32_t addr_bits;
    uint32_t *tx;
    uint32_t tx_bits;
    uint32_t *rx;
    uint32_t rx_bits;
    uint32_t dummy_bits;
} rom_cmd_t;
#define ROM_SPI_CMD_CONFIG(n, c)   ((void (*)(int, rom_cmd_t *))0x4fc00108)((n), (c))
#define ROM_SPI_CMD_START(n, rx, len, cs, wr) ((void (*)(int, uint8_t *, uint16_t, uint8_t, bool))0x4fc0010c)((n), (rx), (len), (cs), (wr))
#define ROM_SPI_SET_OP_MODE(n, m)  ((void (*)(int, int))0x4fc00110)((n), (m))
#define ROM_OPI_DTR_MODE 7

static uint32_t size_bytes;
int psram_line_bytes = 64;                          /* kept for API parity with the S3 (cache line is 64 B here) */

/* ---- supply: PMU external LDO channel 2 at 1.8 V (IDF ESP_LDO_CHAN_PSRAM_DOMAIN, ldo_ll.h; EV board wiring) ---- */
#define LDO_UNIT_REG(u)  (PMU_BASE + 0x1B8 + 8 * ((const uint8_t[]){0, 3, 1, 4}[u]))   /* unit = channel - 1 */
#define EFUSE_SYS_2     (SOC_EFUSE_BASE + 0x4C)         /* b31:28 ldo_vo2_dref */
#define EFUSE_SYS_3     (SOC_EFUSE_BASE + 0x50)         /* b5:3 ldo_vo2_mul */
#ifndef NDF_PSRAM_LDO_CHAN
#define NDF_PSRAM_LDO_CHAN 2                        /* 0: the board powers the PSRAM externally */
#endif
#ifndef NDF_PSRAM_LDO_MV
#define NDF_PSRAM_LDO_MV   1800
#endif

static void ldo_on(int unit, int mv)
{
    int best = 0x7FFFFFFF;
    uint32_t dref = 0, mul = 0;
    for (uint32_t d = 0; d < 16; d++) {
        int vref20 = d < 9 ? 10 + (int)d : 20 + ((int)d - 9) * 2;
        for (uint32_t m = 0; m < 8; m++) {
            int err = mv * 80000 - vref20 * 1000 * (4000 + (int)m * 1000);   /* no per-chip trim for this unit */
            if (err < 0) err = -err;
            if (err < best) { best = err; dref = d; mul = m; }
        }
    }
    uint32_t blk = ((REG(EFUSE_SYS_2) >> 11) & 3) * 100 + ((REG(EFUSE_SYS_2) >> 8) & 7);
    if (blk >= 1 && unit == 1 && mv == 1800 && (REG(EFUSE_SYS_2) >> 28) && ((REG(EFUSE_SYS_3) >> 3) & 7)) {
        dref = REG(EFUSE_SYS_2) >> 28;               /* factory-trimmed values */
        mul = (REG(EFUSE_SYS_3) >> 3) & 7;
    }
    uint32_t ctrl = LDO_UNIT_REG(unit), ana = ctrl + 4;
    REG_SET(ana, BIT(27));                          /* current limit on */
    REG_FIELD(ana, 28, 4, dref);
    REG_FIELD(ana, 23, 3, mul);
    REG_CLR(ctrl, BIT(14));                         /* tieh: not the 3.3 V rail */
    REG_SET(ctrl, BIT(7)); REG_FIELD(ctrl, 9, 3, 0);   /* software owner, tieh_sel 0 */
    REG_SET(ana, BIT(26));                          /* ripple suppression */
    REG_SET(ctrl, BIT(8));                          /* xpd */
    REG_CLR(ana, BIT(27));
    for (uint32_t t = ccount(); ccount() - t < 200u * NDF_CPU_MHZ; ) {}   /* stable delay: 200 us */
}

/* ---- MPLL 320 MHz (clk_ll_mpll_set_config_v3) ---- */
static void mpll_on(uint32_t mhz)
{
    REG_SET(PMU_RF_PWC, BIT(24));
    REG_SET(LP_HP_CLK_CTRL, BIT(28));
    regi2c_write_mask(I2C_MPLL, 1, 1, 0, 3);        /* IR_CAL_EXT_CAP */
    regi2c_write_mask(I2C_MPLL, 1, 2, 2, 1);        /* IR_CAL_ENX_CAP */
    regi2c_write_mask(I2C_MPLL, 2, 7, 3, 9);        /* DIV */
    regi2c_write_mask(I2C_MPLL, 3, 5, 4, 3);        /* DHREF */
    regi2c_write_mask(I2C_MPLL, 1, 2, 2, 0);
    REG_CLR(ANA_PLL_CTRL0, BIT(9));                 /* calibration start */
    regi2c_write_mask(I2C_MPLL, 2, 7, 3, (uint8_t)(mhz / 20 - 1));
    for (uint32_t t = ccount(); !(REG(ANA_PLL_CTRL0) & BIT(8)) && ccount() - t < 100u * 1000 * NDF_CPU_MHZ; ) {}
    REG_SET(ANA_PLL_CTRL0, BIT(9));                 /* calibration stop */
}

/* ---- pad drive and DQS (mspi_timing_ll_pin_drv_set / _enable_dqs): drive field b13:12 (DQS: b16:15) ---- */
void psram_pins(void)
{
    static const uint8_t data[] = { 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x40, 0x44,
                                    0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c, 0x60, 0x64 };   /* D..DQ7, CK, CS, DQ8..DQ15 */
    for (unsigned i = 0; i < sizeof data; i++) REG_FIELD(IOMUX_MSPI + data[i], 12, 2, 2);
    REG_FIELD(IOMUX_MSPI + 0x3C, 15, 2, 2); REG_FIELD(IOMUX_MSPI + 0x68, 15, 2, 2);     /* DQS0 / DQS1 */
    REG_SET(IOMUX_MSPI + 0x3C, BIT(0)); REG_SET(IOMUX_MSPI + 0x68, BIT(0));              /* DQS input powered */
}

/* one 16-bit-command register / data transaction on MSPI1, octal DDR, CS1 (psram_ctrlr_ll_common_transaction) */
int psram_cmd(uint16_t cmd, uint32_t addr, int dummy, const uint8_t *out, int nout, uint8_t *in, int nin)
{
    uint32_t txw[2] = { 0, 0 }, rxw[2] = { 0, 0 };
    for (int i = 0; i < nout && i < 8; i++) ((uint8_t *)txw)[i] = out[i];
    rom_cmd_t c = { .cmd = cmd, .cmd_bits = 16, .addr = &addr, .addr_bits = 32, .tx = txw, .tx_bits = (uint32_t)nout * 8,
                    .rx = rxw, .rx_bits = (uint32_t)nin * 8, .dummy_bits = (uint32_t)dummy };
    ROM_SPI_SET_OP_MODE(3, ROM_OPI_DTR_MODE);
    ROM_SPI_CMD_CONFIG(3, &c);
    ROM_SPI_CMD_START(3, (uint8_t *)rxw, (uint16_t)nin, BIT(1), false);
    for (int i = 0; i < nin && i < 8; i++) in[i] = ((uint8_t *)rxw)[i];
    return 0;
}

int psram_probe(uint8_t mr[8])
{
    psram_pins();
    for (int a = 0; a < 8; a += 2) psram_cmd(CMD_REG_RD, (uint32_t)a, RD_REG_DUMMY, 0, 0, mr + a, 2);
    return 0;
}

/* AXI (cache) path of MSPI0: commands, address, dummies and the octal / hex DDR mode (s_config_mspi_for_psram) */
void psram_spi0_setup(int clk_div, int rd_dummy, int wr_dummy)
{
    uint32_t sc = REG(SCTRL);
    sc &= ~(0x3Fu << 22 | 0x3Fu << 14 | 0x3Fu << 6);
    sc |= BIT(20) | BIT(5) | BIT(0) | BIT(4) | BIT(3) | BIT(21) | 31u << 14 | (uint32_t)(rd_dummy - 1) << 6 | (uint32_t)(wr_dummy - 1) << 22;
    REG(SCTRL) = sc;
    REG(SRAM_DWR) = 15u << 28 | CMD_SYNC_WR;
    REG(SRAM_DRD) = 15u << 28 | CMD_SYNC_RD;
    REG(SMEM_DDR) = (REG(SMEM_DDR) & ~(BIT(2) | BIT(3))) | BIT(0) | BIT(1);          /* DDR, variable dummy, no swap */
    REG(SRAM_CMD) |= BIT(23) | BIT(21) | BIT(20) | BIT(19) | BIT(18) | BIT(27) | BIT(26);   /* dummy_wout, oct cmd / addr / dout / din, hex data */
    REG(CTRL1) |= BIT(25) | BIT(26);                                                  /* ar / aw splice */
    REG(FCTRL) = (REG(FCTRL) | BIT(0)) & ~BIT(31);                                    /* AXI requests on */
    uint32_t n = (uint32_t)clk_div - 1;
    REG(SRAM_CLK) = n << 16 | ((uint32_t)clk_div / 2 - 1) << 8 | n;
}

/* bench: MR0..MR2 read time in cycles (the S3 build returns the flash JEDEC id here) */
uint32_t psram_bench_jedec(void)
{
    uint8_t b[2];
    uint32_t t = ccount();
    for (int i = 0; i < 64; i++) psram_cmd(CMD_REG_RD, 0, RD_REG_DUMMY, 0, 0, b, 2);
    return (ccount() - t) / 64;
}

void psram_writeback(void *p, size_t n) { cache_dma_writeback(p, n); }
void psram_invalidate(void *p, size_t n) { cache_dma_wbinv(p, n); }
uint32_t psram_size(void) { return size_bytes; }

uint32_t psram_init(int clk_div)
{
    if (clk_div < 4) clk_div = 4;                   /* 80 MHz is the fastest untuned setting */
    if (NDF_PSRAM_LDO_CHAN) ldo_on(NDF_PSRAM_LDO_CHAN - 1, NDF_PSRAM_LDO_MV);
    mpll_on(MPLL_MHZ);
    REG_SET(SOC_CLK_CTRL0, BIT(31));
    REG_SET(PERI_CLK_CTRL00, BIT(14) | BIT(15));
    REG_SET(HP_RST_EN0, BIT(23) | BIT(25)); REG_CLR(HP_RST_EN0, BIT(25) | BIT(23));
    REG_FIELD(PERI_CLK_CTRL00, 12, 2, 1);           /* source: MPLL */
    REG_FIELD(PERI_CLK_CTRL00, 16, 8, 0);           /* core clock = MPLL (div 1) */
    psram_pins();
    REG(SMEM_AC) = (REG(SMEM_AC) & ~(0x1Fu << 2 | 0x1Fu << 7 | 0x3Fu << 25)) | BIT(0) | BIT(1) | (4u - 1) << 2 | (4u - 1) << 7 | (3u - 1) << 25 | BIT(31);   /* CS setup / hold 4, hold delay 3, split trans */
    REG_FIELD(SMEM_ECC, 18, 2, 3);                  /* page size 2048 */
    uint32_t n = (uint32_t)clk_div - 1, bus = n << 16 | ((uint32_t)clk_div / 2 - 1) << 8 | n;
    REG(SRAM_CLK) = bus;                            /* MSPI0 */
    REG(M1(0x14)) = bus;                            /* MSPI1 */
    REG_SET(SMEM_CALI, BIT(5));                     /* DLL on, both sides (IDF enables it at every speed) */
    REG_SET(M0(0x180), BIT(5));

    /* mode registers: MR0 latency (fixed, read latency 2), MR4 write latency, MR8 burst 2048 hybrid wrap, x16 (hex) */
    uint8_t mr0[2], mr4[2], mr8[2], t[2];
    psram_cmd(CMD_REG_RD, 0, RD_REG_DUMMY, 0, 0, mr0, 2);
    psram_cmd(CMD_REG_RD, 4, RD_REG_DUMMY, 0, 0, mr4, 2);
    mr0[0] = (uint8_t)((mr0[0] & 0x40) | 0u | RD_LATENCY << 2 | 1u << 5);       /* drive 0, RL, LT = fixed */
    mr4[0] = (uint8_t)((mr4[0] & 0x1F) | WR_LATENCY << 5);
    psram_cmd(CMD_REG_WR, 0, 0, mr0, 2, 0, 0);
    psram_cmd(CMD_REG_WR, 4, 0, mr4, 2, 0, 0);
    psram_cmd(CMD_REG_RD, 8, RD_REG_DUMMY, 0, 0, mr8, 1);
    mr8[0] = (uint8_t)((mr8[0] & ~0x4Fu) | 3u | 0u << 2 | 1u << 3 | 1u << 6);   /* bl 3 (2048), bt 0, rbx 1, x16 */
    mr8[1] = 0;
    psram_cmd(CMD_REG_WR, 8, 0, mr8, 2, 0, 0);

    /* connectivity: write / read back a known word in register mode */
    uint32_t ref = REF_DATA, got = 0;
    psram_cmd(CMD_SYNC_WR, 0, WR_DUMMY, (const uint8_t *)&ref, 4, 0, 0);
    psram_cmd(CMD_SYNC_RD, 0, RD_DUMMY, 0, 0, (uint8_t *)&got, 4);
    if (got != ref) return 0;
    psram_cmd(CMD_REG_RD, 2, RD_REG_DUMMY, 0, 0, t, 2);                         /* MR2: vendor / density */
    static const uint32_t sizes[8] = { 0, 4u << 20, 0, 8u << 20, 0, 16u << 20, 64u << 20, 32u << 20 };
    uint32_t dens = (uint32_t)(t[0] & 7);
    size_bytes = sizes[dens];
    if (!size_bytes) return 0;

    psram_spi0_setup(clk_div, 10, 5);
    REG(SMEM_DDR) |= BIT(1);                        /* variable dummy */
    /* PSRAM MMU: page i of the PSRAM at 0x48000000 + 64 KB * i; stale cache lines of that window dropped afterwards */
    uint32_t pages = size_bytes >> 16;
    if (pages > MMU_ENTRIES) pages = MMU_ENTRIES;
    for (uint32_t i = 0; i < pages; i++) {
        REG(MMU_INDEX) = i;
        REG(MMU_CONTENT) = i | PSRAM_MMU_VALID | PSRAM_MMU_ACCESS;
    }
    size_bytes = pages << 16;
    cache_dma_wbinv((void *)PSRAM_VADDR, size_bytes);
    return size_bytes;
}
