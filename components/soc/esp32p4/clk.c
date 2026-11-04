/* ESP32-P4 (v3.x) clock tree, same API as soc/esp32s3/clk.c. Register sequence from ESP-IDF v6.1
 * esp_hw_support/port/esp32p4/rtc_clk_init.c + rtc_clk.c and esp_hal_clock/esp32p4 clk_tree_ll.h; no ROM calls.
 * CPU from CPLL 400 MHz: CPU 400 / MEM 200 / SYS 200 / APB 100 (div 1/2/1/2), 200 (1/1/1/2), 100 (1/1/1/1); 40 = XTAL.
 * Analog registers (CPLL dividers) go through the LP analog I2C master (IDF esp_hal_regi2c/esp32p4/regi2c_impl.c). */
#include "clk.h"
#include "soc.h"
#include "cpu.h"
#include "spin.h"

#define HPC(off)              (HP_CLKRST_BASE + (off))
#define ROOT_CLK_CTRL0        HPC(0x04)     /* b4 SOC_CLK_DIV_UPDATE, b12:5 CPU div-1, b20:13 num, b28:21 den */
#define ROOT_CLK_CTRL1        HPC(0x08)     /* b7:0 MEM div-1, b31:24 SYS div-1 */
#define ROOT_CLK_CTRL2        HPC(0x0C)     /* b23:16 APB div-1 */
#define ANA_PLL_CTRL0         HPC(0xBC)     /* b2 CPU_PLL_CAL_END, b3 CPU_PLL_CAL_STOP */
#define LP_HP_CLK_CTRL        (LP_CLKRST_BASE + 0x40)   /* b1:0 HP root source: 0 XTAL, 1 CPLL, 2 RC_FAST */
#define PMU_IMM_HP_CK_POWER   (PMU_BASE + 0xCC)         /* b27 XPD_CPLL, b23 XPD_CPLL_I2C, b17 GLOBAL_CPLL_ICG */
#define PMU_HP_ACT_BIAS       (PMU_BASE + 0x18)         /* b22:18 DCM_VSET */
#define PMU_HP_ACT_REG0       (PMU_BASE + 0x28)         /* b31:27 DBIAS, b18 XPD, b14 DBIAS_SEL, b13:9 DBIAS_VOL (RO) */
#define PMU_LP_ACT_REG0       (PMU_BASE + 0x9C)         /* lp_sys[LP_ACTIVE].regulator0: b31:27 DBIAS */
#define PMU_DCDC_SWITCH       (PMU_BASE + 0x10C)        /* b0 force PU, b1 force PD */
#define PMU_DCM_CTRL          (PMU_BASE + 0x204)        /* b0 ON_REQ, b1 OFF_REQ, b7 DONE_FORCE, b11 FB_RES_FORCE_PD */
#define LP_SYS_SYS_CTRL       (LP_SYS_BASE + 0x08)      /* b28:21 LP_FIB_SEL */
#define EFUSE_BASE            (LPPERIPH + 0xD000)
#define EFUSE_RD_MAC_SYS_2    (EFUSE_BASE + 0x4C)       /* b3:0 wafer minor, b5:4 major lo, b23 major hi, b10:8 blk minor, b12:11 blk major */
#define EFUSE_RD_MAC_SYS_4    (EFUSE_BASE + 0x54)       /* b19:16 ACTIVE_HP_DBIAS, b23:20 ACTIVE_LP_DBIAS */

#define LP_ANA_MST            (LPPERIPH + 0x4000)
#define ANA_I2C0_CTRL         (LP_ANA_MST + 0x00)       /* b7:0 slave, b15:8 addr, b23:16 data, b24 write, b25 busy */
#define ANA_CONF1             (LP_ANA_MST + 0x1C)
#define ANA_CONF2             (LP_ANA_MST + 0x20)
#define ANA_CLK160M           (LP_ANA_MST + 0x34)
#define LPPERI_CLK_EN         (LPPERIPH + 0x0000)       /* b27 LP_I2CMST */

#define I2C_DIG_REG 0x6D
#define I2C_BIAS    0x6A
#define I2C_CPLL    0x67

static uint32_t cpu_mhz = 40, apb_mhz = 10;   /* power-on: XTAL 40, MEM 20, SYS 20, APB 10 */

static uint32_t chip_rev(void)                 /* major * 100 + minor (IDF efuse_hal_chip_revision) */
{
    uint32_t r = REG(EFUSE_RD_MAC_SYS_2);
    return (((r >> 23) & 1) << 2 | ((r >> 4) & 3)) * 100 + (r & 15);
}

static uint32_t blk_version(void) { uint32_t r = REG(EFUSE_RD_MAC_SYS_2); return ((r >> 11) & 3) * 100 + ((r >> 8) & 7); }

/* ---- analog register bus ------------------------------------------------ */
static void ana_select(uint8_t block)
{
    static const struct { uint8_t blk, bit; } sel[] = { { I2C_DIG_REG, 10 }, { I2C_CPLL, 11 }, { I2C_BIAS, 12 } };
    REG(ANA_CONF2) = 0;
    REG(ANA_CONF1) = 0;
    for (unsigned i = 0; i < sizeof sel / sizeof sel[0]; i++) if (sel[i].blk == block) REG_SET(ANA_CONF2, BIT(sel[i].bit));
}

static uint8_t ana_rd(uint8_t block, uint8_t addr)
{
    ana_select(block);
    SPIN_US(!(REG(ANA_I2C0_CTRL) & BIT(25)), 1000);
    REG(ANA_I2C0_CTRL) = block | (uint32_t)addr << 8;
    SPIN_US(!(REG(ANA_I2C0_CTRL) & BIT(25)), 1000);
    return (uint8_t)(REG(ANA_I2C0_CTRL) >> 16);
}

static void ana_wr(uint8_t block, uint8_t addr, uint8_t v)
{
    ana_select(block);
    SPIN_US(!(REG(ANA_I2C0_CTRL) & BIT(25)), 1000);
    REG(ANA_I2C0_CTRL) = block | (uint32_t)addr << 8 | BIT(24) | (uint32_t)v << 16;
    SPIN_US(!(REG(ANA_I2C0_CTRL) & BIT(25)), 1000);
}

static void ana_wr_bits(uint8_t block, uint8_t addr, int msb, int lsb, uint8_t v)
{
    uint8_t m = (uint8_t)(((1u << (msb - lsb + 1)) - 1) << lsb);
    ana_wr(block, addr, (uint8_t)((ana_rd(block, addr) & ~m) | ((v << lsb) & m)));
}

static void ana_bus_on(void)
{
    REG_SET(LPPERI_CLK_EN, BIT(27));
    REG_SET(ANA_CLK160M, BIT(0));
}

/* ---- regulators: hand the digital supply to the PMU / DCDC at the calibrated voltage (rtc_clk_init) ---------- */
static void spin_us(uint32_t us) { uint32_t t = ccount(), n = us * cpu_mhz; while (ccount() - t < n) {} }

static void regulators_init(void)
{
    ana_wr_bits(I2C_DIG_REG, 10, 0, 0, 1);       /* FORCE_RTC_DREG */
    ana_wr_bits(I2C_DIG_REG, 10, 1, 1, 1);       /* FORCE_DIG_DREG */
    ana_wr_bits(I2C_DIG_REG, 13, 2, 2, 0);       /* XPD_RTC_REG */
    ana_wr_bits(I2C_DIG_REG, 13, 3, 3, 0);       /* XPD_DIG_REG */
    ana_wr_bits(I2C_BIAS, 4, 3, 0, 0);           /* OR_FORCE_XPD_CK / REF_OUT_BUF / IPH / VGATE_BUF */

    uint32_t hp = 24, lp = 29, bv = blk_version();               /* defaults: HP 24, LP 29 (IDF pmu_param.h) */
    if (bv >= 2 && bv != 100) {
        uint32_t e = REG(EFUSE_RD_MAC_SYS_4), eh = (e >> 16) & 15, el = (e >> 20) & 15;
        if (eh) hp = eh + 16 > 31 ? 31 : eh + 16;
        if (el) lp = el + 20 > 31 ? 31 : el + 20;
    }
    REG_SET(PMU_HP_ACT_REG0, BIT(18));                           /* HP regulator on */
    REG_FIELD(PMU_HP_ACT_REG0, 27, 5, hp);
    REG_FIELD(PMU_LP_ACT_REG0, 27, 5, lp);
    uint32_t vset = (REG(PMU_HP_ACT_REG0) >> 9) & 31;
    if (vset < 27) vset = 27;                                    /* HP_CALI_ACTIVE_DCM_VSET_DEFAULT: about 1.25 V */
    bool rev301 = chip_rev() >= 301;
    if (rev301) REG_SET(PMU_DCM_CTRL, BIT(11));
    REG_CLR(PMU_DCM_CTRL, BIT(7)); REG_SET(PMU_DCM_CTRL, BIT(0)); /* DCDC on */
    REG_CLR(PMU_DCDC_SWITCH, BIT(0) | BIT(1));
    REG_FIELD(PMU_HP_ACT_BIAS, 18, 5, vset);
    REG_SET(PMU_HP_ACT_REG0, BIT(14));                           /* dbias under PMU control */
    spin_us(1000);
    if (rev301) {
        REG_FIELD(LP_SYS_SYS_CTRL, 21, 8, 0xEF);                 /* dig_fib_reg instead of ana_fib_reg */
        REG_CLR(PMU_DCM_CTRL, BIT(11));
        spin_us(10);
    }
    REG_CLR(PMU_HP_ACT_REG0, BIT(18));                           /* the DCDC supplies the core now */
}

/* ---- CPLL ---------------------------------------------------------------- */
static void cpll_on(uint32_t mhz)
{
    REG_SET(PMU_IMM_HP_CK_POWER, BIT(27) | BIT(23));
    REG_SET(PMU_IMM_HP_CK_POWER, BIT(17));
    REG_CLR(ANA_PLL_CTRL0, BIT(3));                              /* calibration start */
    uint8_t div = chip_rev() >= 100 ? (mhz == 400 ? 10 : 9) : (mhz == 400 ? 6 : 5);   /* rev >= 1: f = 40 * div */
    ana_wr(I2C_CPLL, 2, (uint8_t)(5 << 4));                      /* REF_DIV: dchgp 5, ref div 0, fcal on */
    ana_wr(I2C_CPLL, 3, div);                                    /* DIV_7_0 */
    ana_wr(I2C_CPLL, 6, (uint8_t)(1 << 6 | 3 << 4 | 3));         /* DCUR: dlref 1, dhref 3, dcur 3 */
    SPIN_US(REG(ANA_PLL_CTRL0) & BIT(2), 10000);
    spin_us(10);
    REG_SET(ANA_PLL_CTRL0, BIT(3));                              /* calibration stop */
}

static void bus_update(void)
{
    REG_SET(ROOT_CLK_CTRL0, BIT(4));
    SPIN_US(!(REG(ROOT_CLK_CTRL0) & BIT(4)), 100);
}

static void dividers(uint32_t cpu, uint32_t mem, uint32_t sys, uint32_t apb, bool up)
{
    if (up) {           /* upscaling: APB first (IDF: no illegal intermediate state) */
        REG_FIELD(ROOT_CLK_CTRL2, 16, 8, apb - 1); bus_update();
        REG_FIELD(ROOT_CLK_CTRL1, 24, 8, sys - 1); bus_update();
        REG_FIELD(ROOT_CLK_CTRL1, 0, 8, mem - 1);  bus_update();
        REG_FIELD(ROOT_CLK_CTRL0, 5, 24, cpu - 1); bus_update();   /* integer, numerator 0, denominator 0 */
    } else {
        REG_FIELD(ROOT_CLK_CTRL0, 5, 24, cpu - 1); bus_update();
        REG_FIELD(ROOT_CLK_CTRL1, 0, 8, mem - 1);  bus_update();
        REG_FIELD(ROOT_CLK_CTRL1, 24, 8, sys - 1); bus_update();
        REG_FIELD(ROOT_CLK_CTRL2, 16, 8, apb - 1); bus_update();
    }
}

int clk_cpu_set_mhz(uint32_t mhz)
{
    uint32_t mem, apb;
    switch (mhz) {
    case 400: mem = 2; apb = 2; break;
    case 200: mem = 1; apb = 2; break;
    case 100: mem = 1; apb = 1; break;
    case 40:
        REG_FIELD(LP_HP_CLK_CTRL, 0, 2, 0);                      /* source first (IDF rtc_clk_cpu_freq_to_xtal) */
        dividers(1, 1, 1, 1, false);
        cpu_mhz = 40; apb_mhz = 40;
        return 0;
    default: return -1;
    }
    static bool pll_on, regs_on;
    ana_bus_on();
    if (!regs_on) { regulators_init(); regs_on = true; }
    if (!pll_on) { cpll_on(400); pll_on = true; }
    uint32_t old = cpu_mhz, c = ccount();
    dividers(400 / mhz, mem, 1, apb, mhz >= cpu_mhz);
    REG_FIELD(LP_HP_CLK_CTRL, 0, 2, 1);                          /* CPLL last */
    cpu_mhz = mhz; apb_mhz = mhz / mem / apb;
    __asm__ volatile("csrw mcycle, %0" :: "r"((uint32_t)((uint64_t)c * mhz / old)));   /* keep ccount() time-consistent */
    return 0;
}

uint32_t clk_cpu_hz(void) { return cpu_mhz * 1000000u; }
uint32_t clk_apb_hz(void) { return apb_mhz * 1000000u; }

uint32_t clk_cpu_sync(void)
{
    uint32_t src = REG(LP_HP_CLK_CTRL) & 3, div = ((REG(ROOT_CLK_CTRL0) >> 5) & 0xFF) + 1;
    uint32_t mem = (REG(ROOT_CLK_CTRL1) & 0xFF) + 1, sys = (REG(ROOT_CLK_CTRL1) >> 24) + 1, apb = ((REG(ROOT_CLK_CTRL2) >> 16) & 0xFF) + 1;
    uint32_t root = src == 1 ? 400 : src == 0 ? 40 : 20;
    cpu_mhz = root / div; apb_mhz = cpu_mhz / mem / sys / apb;
    return clk_cpu_hz();
}

/* ---- peripheral gates: same periph_t ids as the S3; P4 bus clock / function clock / reset bits per id ----------- */
typedef struct { uint16_t bus_reg; uint8_t bus_bit; uint16_t fn_reg; uint32_t fn_mask; uint16_t rst_reg; uint32_t rst; } gate_t;
#define CRYPTO_BUS 0x18, 26                   /* SOC_CLK_CTRL1 b26: crypto system clock */
#define CRYPTO_FN  0xA8                       /* PERI_CLK_CTRL25: b14 AES, 15 DS, 16 ECC, 17 HMAC, 18 RSA, 19 SEC, 20 SHA, 21 ECDSA */
#define CRYPTO_RST 0xC8                       /* HP_RST_EN2: b14 crypto, 16 AES, 17 DS, 18 SHA, 19 HMAC, 20 ECDSA, 21 RSA, 22 ECC */
static const gate_t *gate(periph_t p)
{
    static const gate_t uart0 = { 0x1C, 7, 0x68, BIT(26), 0xC4, BIT(8) | BIT(13) }, uart1 = { 0x1C, 8, 0x6C, BIT(26), 0xC4, BIT(9) | BIT(14) },
        timg0 = { 0x1C, 21, 0x94, BIT(24), 0xC4, BIT(6) }, timg1 = { 0x1C, 22, 0x98, BIT(22), 0xC4, BIT(7) },
        stimer = { 0x1C, 23, 0x98, BIT(30), 0xC4, BIT(5) },
        aes = { CRYPTO_BUS, CRYPTO_FN, BIT(14) | BIT(19), CRYPTO_RST, BIT(14) | BIT(16) },
        sha = { CRYPTO_BUS, CRYPTO_FN, BIT(20) | BIT(19), CRYPTO_RST, BIT(14) | BIT(18) },
        rsa = { CRYPTO_BUS, CRYPTO_FN, BIT(18) | BIT(19), CRYPTO_RST, BIT(14) | BIT(21) },
        ds = { CRYPTO_BUS, CRYPTO_FN, BIT(15) | BIT(19), CRYPTO_RST, BIT(14) | BIT(17) },
        hmac = { CRYPTO_BUS, CRYPTO_FN, BIT(17) | BIT(19), CRYPTO_RST, BIT(14) | BIT(19) };
    switch (p) {
    case PERIPH_UART0: return &uart0;
    case PERIPH_UART1: return &uart1;
    case PERIPH_TIMG0: return &timg0;
    case PERIPH_TIMG1: return &timg1;
    case PERIPH_SYSTIMER: return &stimer;
    case PERIPH_AES: return &aes;
    case PERIPH_SHA: return &sha;
    case PERIPH_RSA: return &rsa;
    case PERIPH_DS: return &ds;
    case PERIPH_HMAC: return &hmac;
    default: return 0;
    }
}

void periph_enable(periph_t p)
{
    const gate_t *g = gate(p);
    if (!g) return;
    uint32_t t = irq_lock();
    REG_SET(HPC(g->bus_reg), BIT(g->bus_bit));
    REG_SET(HPC(g->fn_reg), g->fn_mask);
    REG_CLR(HPC(g->rst_reg), g->rst);
    irq_unlock(t);
}

void periph_disable(periph_t p)
{
    const gate_t *g = gate(p);
    if (!g) return;
    uint32_t t = irq_lock();
    REG_SET(HPC(g->rst_reg), g->rst);
    REG_CLR(HPC(g->fn_reg), g->fn_mask);
    REG_CLR(HPC(g->bus_reg), BIT(g->bus_bit));
    irq_unlock(t);
}

void periph_reset(periph_t p)
{
    const gate_t *g = gate(p);
    if (!g) return;
    uint32_t t = irq_lock();
    REG_SET(HPC(g->rst_reg), g->rst);
    REG_CLR(HPC(g->rst_reg), g->rst);
    irq_unlock(t);
}

int clk_out(int ch, int src, int pin) { (void)ch; (void)src; (void)pin; return -1; }   /* ponytail: no clock output on the P4 yet */
