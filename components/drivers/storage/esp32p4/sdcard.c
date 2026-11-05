/* P4 SD/MMC host + SD card protocol (sdcard.h). Host: flow of ESP-IDF v6.1 esp_driver_sdmmc/src/sd_host_sdmmc.c and
 * esp_hal_sd/esp32p4/sdmmc_ll.h (bus clock + reset in LP_CLKRST, PLL_F160M through the HP_SYS_CLKRST edge divider, card
 * clock divider, controller / DMA / FIFO reset, internal DMA with chained descriptors, command start with "use hold register").
 * Card: the SD Physical Layer sequence (CMD0, CMD8, ACMD41, CMD2, CMD3, CMD9, CMD7, ACMD6, CMD6 high speed, CMD16) written from
 * the specification. Registers: DWC mobile storage layout (regs/sdmmc.h). NOT VERIFIED ON HARDWARE. */
#include "sdcard.h"
#include "ldo.h"
#include "clk.h"
#include "cpu.h"
#include "spin.h"
#include "soc.h"
#include "cache_p4.h"
#include "regs/sdmmc.h"
#include "regs/hp_clkrst.h"
#include "regs/lp_clkrst.h"
#include "regs/p4reg.h"
#include <string.h>

#ifndef NDF_SD_LDO_CHAN
#define NDF_SD_LDO_CHAN 4
#endif
#define H(r)  SDHOST_##r##_REG
/* CMD register bits */
#define C_RESP     BIT(6)
#define C_LONG     BIT(7)
#define C_CRC      BIT(8)
#define C_DATA     BIT(9)
#define C_WRITE    BIT(10)
#define C_AUTOSTOP BIT(12)
#define C_WAITPRV  BIT(13)
#define C_INIT     BIT(15)
#define C_UPDCLK   BIT(21)
#define C_HOLD     BIT(29)
#define C_START    BIT(31)
/* RINTSTS bits */
#define I_CD    BIT(2)
#define I_DTO   BIT(3)
#define I_RCRC  BIT(6)
#define I_DCRC  BIT(7)
#define I_RTO   BIT(8)
#define I_DRTO  BIT(9)
#define I_HLE   BIT(12)
#define I_SBE   BIT(13)
#define I_EBE   BIT(15)
#define I_RE    BIT(1)
#define I_ERR   (I_RE | I_RCRC | I_DCRC | I_RTO | I_DRTO | I_HLE | I_SBE | I_EBE)

typedef struct { uint32_t des0, des1, des2, des3; } idmac_t;
static idmac_t chain[64] __attribute__((aligned(16)));
static struct { bool up, hc; uint32_t rca, sectors; int width; uint32_t src_khz; } c;

static int cmd_start(uint32_t cmd, uint32_t arg)
{
    SPIN_US(REG(H(CMD)) & C_START, 100000);
    if (REG(H(CMD)) & C_START) return -1;
    REG(H(CMDARG)) = arg;
    REG(H(CMD)) = cmd | C_HOLD | C_START;
    SPIN_US(REG(H(CMD)) & C_START, 100000);
    return REG(H(CMD)) & C_START ? -1 : 0;
}
static int clock_update(void) { return cmd_start(C_UPDCLK | C_WAITPRV, 0); }

static int set_clock(uint32_t khz)
{
    REG_CLR(H(CLKENA), BIT(0));                                              /* card clock off while changing */
    if (clock_update()) return -1;
    /* 160 MHz PLL / host divider (2..16) / (2 * card divider); the same plan as sd_host_slot_get_clk_dividers */
    uint32_t hd, cd;
    if (khz >= 50000) { hd = 4; cd = 0; }
    else if (khz >= 25000) { hd = 8; cd = 0; }
    else { hd = 10; cd = (160000 / hd + 2 * khz - 1) / (2 * khz); if (cd > 255) cd = 255; }
    FPUT(HP_SYS_CLKRST_REG_SDIO_LS_CLK_SRC_SEL, 0);                          /* PLL_F160M */
    FSET(HP_SYS_CLKRST_REG_SDIO_LS_CLK_EN);
    if (hd > 1) {
        FPUT(HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_H, hd / 2 - 1); FPUT(HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_N, hd - 1); FPUT(HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_L, hd - 1);
        FSET(HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_CFG_UPDATE); FCLR(HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_CFG_UPDATE);
    }
    FSET(HP_SYS_CLKRST_REG_SDIO_LS_DRV_CLK_EN); FSET(HP_SYS_CLKRST_REG_SDIO_LS_SAM_CLK_EN); FSET(HP_SYS_CLKRST_REG_SDIO_LS_SLF_CLK_EN);
    FPUT(HP_SYS_CLKRST_REG_SDIO_LS_DRV_CLK_EDGE_SEL, 1); FPUT(HP_SYS_CLKRST_REG_SDIO_LS_SAM_CLK_EDGE_SEL, 0); FPUT(HP_SYS_CLKRST_REG_SDIO_LS_SLF_CLK_EDGE_SEL, 0);
    FSET(HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_CFG_UPDATE); FCLR(HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_CFG_UPDATE);
    REG(H(CLKSRC)) = 0;
    REG(H(CLKDIV)) = cd;
    if (clock_update()) return -1;
    REG_SET(H(CLKENA), BIT(0) | BIT(16));                                    /* cclk on, low-power gating when idle */
    if (clock_update()) return -1;
    REG(H(TMOUT)) = 0xFFFFFF00u | 0xFF;                                      /* data timeout max, response 255 clocks */
    return 0;
}

static int host_reset(void)
{
    REG_SET(H(CTRL), BIT(0) | BIT(1) | BIT(2));                              /* controller, FIFO, DMA reset */
    SPIN_US(REG(H(CTRL)) & (BIT(0) | BIT(1) | BIT(2)), 100000);
    return REG(H(CTRL)) & (BIT(0) | BIT(1) | BIT(2)) ? -1 : 0;
}

/* send a command and collect the response; returns 0, or -1 on timeout / CRC error */
static int command(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t resp[4])
{
    REG(H(RINTSTS)) = 0xFFFFFFFFu;
    if (cmd_start(idx | flags | C_WAITPRV, arg)) return -1;
    SPIN_US(!(REG(H(RINTSTS)) & (I_CD | I_ERR)), 100000);
    uint32_t s = REG(H(RINTSTS));
    if (!(s & I_CD) || (s & (I_RTO | I_RE | I_HLE)) || ((s & I_RCRC) && (flags & C_CRC))) return -1;        /* R3 (OCR) carries no valid CRC */
    if (resp && (flags & C_RESP)) { resp[0] = REG(H(RESP0)); resp[1] = REG(H(RESP1)); resp[2] = REG(H(RESP2)); resp[3] = REG(H(RESP3)); }
    return 0;
}
#define R1  (C_RESP | C_CRC)
#define R2  (C_RESP | C_LONG | C_CRC)
#define R3  (C_RESP)

static int run_data(uint32_t cmd, uint32_t arg, void *buf, uint32_t bytes, uint32_t blksz, bool wr, bool autostop);
static int acmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t *resp)
{
    uint32_t r[4];
    if (command(55, c.rca << 16, R1, r)) return -1;
    return command(idx, arg, flags, resp ? resp : r);
}

static int wait_ready(void)                                                  /* card in transfer state, ready for data (CMD13) */
{
    for (int i = 0; i < 100000; i++) {
        uint32_t r[4];
        if (command(13, c.rca << 16, R1, r)) return -1;
        if (r[0] & BIT(8)) return 0;
    }
    return -1;
}

int sdcard_init(const sdcard_cfg_t *cfg)
{
    if (!cfg || (cfg->width != 1 && cfg->width != 4) || cfg->freq_khz < 400 || cfg->freq_khz > 50000) return -1;
    if (NDF_SD_LDO_CHAN) ldo_on(NDF_SD_LDO_CHAN - 1, 3300);
    /* pads: slot 0 dedicated pins, IO MUX function 0 */
    static const uint8_t pins[6] = { 43, 44, 39, 40, 41, 42 };               /* CLK CMD D0 D1 D2 D3 */
    for (int i = 0; i < (cfg->width == 4 ? 6 : 3); i++)
        REG(IO_MUX_BASE + 4 + pins[i] * 4) = 0u << 12 | 3u << 10 | BIT(9);   /* function 0, drive 3, input enable, no pull-down */
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_SDMMC_SYS_CLK_EN);
    FSET(LP_CLKRST_RST_EN_SDMMC); FCLR(LP_CLKRST_RST_EN_SDMMC);
    irq_unlock(t);
    if (host_reset()) return -1;
    REG(H(CTYPE)) = 0;
    REG(H(INTMASK)) = 0;
    REG(H(RINTSTS)) = 0xFFFFFFFFu;
    REG(H(BMOD)) = BIT(0);                                                   /* IDMAC software reset */
    REG(H(IDINTEN)) = 0;
    if (set_clock(400)) return -1;
    uint32_t r[4];
    /* 80 clocks with CMD0 carrying the initialization flag */
    REG(H(RINTSTS)) = 0xFFFFFFFFu;
    cmd_start(0 | C_INIT | C_WAITPRV, 0);
    SPIN_US(!(REG(H(RINTSTS)) & I_CD), 100000);
    if (command(0, 0, 0, 0)) return -1;
    bool v2 = command(8, 0x1AA, R1, r) == 0 && (r[0] & 0xFFF) == 0x1AA;
    uint32_t ocr = 0;
    for (int i = 0; i < 1000; i++) {
        if (acmd(41, 0x00FF8000u | (v2 ? BIT(30) : 0), R3, r)) { if (i > 10) return -1; continue; }
        if (r[0] & BIT(31)) { ocr = r[0]; break; }
        for (volatile int d = 0; d < 20000; d++) {}
    }
    if (!ocr) return -1;
    c.hc = ocr & BIT(30);
    if (command(2, 0, R2, r)) return -1;
    if (command(3, 0, R1, r)) return -1;
    c.rca = r[0] >> 16;
    if (command(9, c.rca << 16, R2, r)) return -1;
    uint32_t csize;
    if ((r[3] >> 30 & 3) == 1) {                                              /* CSD v2: C_SIZE bits 69..48 */
        csize = ((r[2] & 0x3F) << 16) | (r[1] >> 16);
        c.sectors = (csize + 1) * 1024;
    } else {                                                                  /* CSD v1 */
        csize = ((r[2] & 0x3FF) << 2) | (r[1] >> 30);
        uint32_t mult = (r[1] >> 15) & 7, rdbl = (r[2] >> 16) & 15;
        c.sectors = (csize + 1) << (mult + 2) << (rdbl - 9);
    }
    if (command(7, c.rca << 16, R1, r)) return -1;                            /* select */
    if (cfg->width == 4) {
        if (acmd(6, 2, R1, 0)) return -1;
        REG(H(CTYPE)) = BIT(0);
    }
    if (!c.hc) command(16, 512, R1, r);
    uint32_t khz = cfg->freq_khz;
    bool hs = false;
    c.up = true; c.width = cfg->width;
    if (khz > 25000) {                                                        /* CMD6 mode 1, group 1 function 1: high speed (50 MHz) */
        static uint8_t st[64] __attribute__((aligned(16)));
        if (run_data(6, 0x80FFFFF1u, st, 64, 64, false, false) == 0 && (st[16] & 15) == 1) hs = true;
    }
    if (set_clock(hs ? 50000 : (khz > 25000 ? 25000 : khz))) { c.up = false; return -1; }
    return 0;
}

uint32_t sdcard_sectors(void) { return c.sectors; }

static int run_data(uint32_t cmd, uint32_t arg, void *buf, uint32_t bytes, uint32_t blksz, bool wr, bool autostop)
{
    uint32_t nd = (bytes + 4095) / 4096;
    if (nd > 64) return -1;
    for (uint32_t i = 0; i < nd; i++) {
        uint32_t len = bytes - i * 4096 > 4096 ? 4096 : bytes - i * 4096;
        chain[i].des0 = BIT(31) | BIT(4) | (i == 0 ? BIT(3) : 0) | (i == nd - 1 ? BIT(2) | BIT(1) : 0);        /* own, chained, first, last (+ disable interrupt on the others) */
        chain[i].des1 = len;
        chain[i].des2 = (uint32_t)(uintptr_t)((uint8_t *)buf + i * 4096);
        chain[i].des3 = (uint32_t)(uintptr_t)&chain[i + 1];
    }
    cache_dma_writeback(chain, nd * sizeof chain[0]);
    if (wr) cache_dma_writeback(buf, bytes); else cache_dma_wbinv(buf, bytes);
    REG(H(RINTSTS)) = 0xFFFFFFFFu;
    REG(H(IDSTS)) = 0xFFFFFFFFu;
    REG(H(BLKSIZ)) = blksz; REG(H(BYTCNT)) = bytes;
    REG(H(DBADDR)) = (uint32_t)(uintptr_t)&chain[0];
    REG_SET(H(CTRL), BIT(25));                                               /* use internal DMA */
    REG(H(BMOD)) = BIT(7) | BIT(1);                                          /* IDMAC enable, fixed burst */
    uint32_t r[4];
    if (command(cmd, arg, R1 | C_DATA | (wr ? C_WRITE : 0) | (autostop ? C_AUTOSTOP : 0), r)) { REG(H(BMOD)) = 0; return -1; }
    REG(H(PLDMND)) = 1;
    SPIN_US(!(REG(H(RINTSTS)) & (I_DTO | I_ERR)), 2000000);
    uint32_t s = REG(H(RINTSTS));
    REG(H(BMOD)) = 0;
    if (!(s & I_DTO) || (s & (I_DCRC | I_DRTO | I_SBE | I_EBE))) { host_reset(); return -1; }
    if (!wr) cache_dma_wbinv(buf, bytes);
    return 0;
}

static int xfer(uint32_t lba, void *buf, uint32_t n, bool wr)
{
    if (!c.up || !n || n > 65535 || ((uintptr_t)buf & 3)) return -1;
    while (n > 128) { if (xfer(lba, buf, 128, wr)) return -1; lba += 128; buf = (uint8_t *)buf + 128 * 512; n -= 128; }
    if (wait_ready()) return -1;
    return run_data(n > 1 ? (wr ? 25 : 18) : (wr ? 24 : 17), c.hc ? lba : lba * 512, buf, n * 512, 512, wr, n > 1);
}

int sdcard_read(uint32_t lba, void *buf, uint32_t n) { return xfer(lba, buf, n, false); }
int sdcard_write(uint32_t lba, const void *buf, uint32_t n) { return xfer(lba, (void *)buf, n, true); }

static int b_rd(bdev_t *b, uint32_t s, void *buf, uint32_t n) { (void)b; return bdev_range_ok(b, s, n) ? sdcard_read(s, buf, n) : -1; }
static int b_wr(bdev_t *b, uint32_t s, const void *buf, uint32_t n) { (void)b; return bdev_range_ok(b, s, n) ? sdcard_write(s, buf, n) : -1; }
static int b_sync(bdev_t *b) { (void)b; return wait_ready(); }
int bdev_sdcard(bdev_t *b)
{
    if (!c.up) return -1;
    *b = (bdev_t){ 512, c.sectors, b_rd, b_wr, b_sync, 0 };
    return 0;
}
