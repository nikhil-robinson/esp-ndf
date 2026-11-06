/* ESP32-P4 (v3.x) clock tree, same API as soc/esp32s3/clk.c. Register sequence from ESP-IDF v6.1
 * esp_hw_support/port/esp32p4/rtc_clk_init.c + rtc_clk.c and esp_hal_clock/esp32p4 clk_tree_ll.h; no ROM calls.
 * CPU from CPLL 400 MHz: CPU 400 / MEM 200 / SYS 200 / APB 100 (div 1/2/1/2), 200 (1/1/1/2), 100 (1/1/1/1); 40 = XTAL.
 * Analog registers (CPLL dividers) go through the LP analog I2C master (IDF esp_hal_regi2c/esp32p4/regi2c_impl.c). */
#include "clk.h"
#include "soc.h"
#include "cpu.h"
#include "spin.h"
#include "gpio.h"

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
    static const struct { uint8_t blk, bit; } sel[] = { { I2C_DIG_REG, 10 }, { I2C_CPLL, 11 }, { I2C_BIAS, 12 }, { 0x69, 7 } };   /* 0x69 = SAR ADC / TSENS */
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

/* analog register bus for drivers (regi2c.h); the bus clock is switched on first */
uint8_t regi2c_read(uint8_t block, uint8_t reg) { REG_SET(LPPERI_CLK_EN, BIT(27)); REG_SET(ANA_CLK160M, BIT(0)); return ana_rd(block, reg); }
void regi2c_write(uint8_t block, uint8_t reg, uint8_t val) { REG_SET(LPPERI_CLK_EN, BIT(27)); REG_SET(ANA_CLK160M, BIT(0)); ana_wr(block, reg, val); }
void regi2c_write_mask(uint8_t block, uint8_t reg, int msb, int lsb, uint8_t val)
{
    REG_SET(LPPERI_CLK_EN, BIT(27)); REG_SET(ANA_CLK160M, BIT(0));
    ana_wr_bits(block, reg, msb, lsb, val);
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
    mhz = mhz >= 300 ? 400 : mhz >= 150 ? 200 : mhz >= 70 ? 100 : 40;      /* the P4 steps: 400 / 200 / 100 / 40 (an S3 style 160 or 240 lands on the nearest) */
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
    if (!pll_on) { cpll_on(400); pll_on = true; REG_FIELD(HPC(0xA8), 12, 2, 2); }   /* crypto engines (PERI_CLK_CTRL25): clk_sec from the 240 MHz PLL output as esp_crypto_clk_init; the reset value is XTAL, 3x slower engines */
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

/* ---- peripheral gates: same periph_t ids as the S3. Each P4 peripheral has a system / APB bus clock gate (SOC_CLK_CTRL1-3),
 * often a function clock gate (PERI_CLK_CTRLn) and a reset bit (HP_RST_EN0-2): up to four enable masks and one reset mask,
 * offsets from HP_SYS_CLKRST (hw_ver3 hp_sys_clkrst_reg.h). Function clock sources stay at their reset value (XTAL) unless a
 * driver selects another. ---- */
typedef struct { struct { uint16_t reg; uint32_t mask; } en[4]; uint16_t rst_reg; uint32_t rst; } gate_t;
#define CRYPTO(bit, rstbit) { { 0x18, BIT(26) }, { 0xA8, BIT(bit) | BIT(19) } }, 0xC8, BIT(14) | BIT(rstbit)
static const gate_t *gate(periph_t p)
{
    static const gate_t g_uart0 = { { { 0x1C, BIT(7) }, { 0x68, BIT(26) } }, 0xC4, BIT(8) | BIT(13) },
        g_uart1 = { { { 0x1C, BIT(8) }, { 0x6C, BIT(26) } }, 0xC4, BIT(9) | BIT(14) },
        g_timg0 = { { { 0x1C, BIT(21) }, { 0x94, BIT(24) } }, 0xC4, BIT(6) },
        g_timg1 = { { { 0x1C, BIT(22) }, { 0x98, BIT(22) } }, 0xC4, BIT(7) },
        g_stimer = { { { 0x1C, BIT(23) }, { 0x98, BIT(30) } }, 0xC4, BIT(5) },
        g_aes = { CRYPTO(14, 16) }, g_sha = { CRYPTO(20, 18) }, g_rsa = { CRYPTO(18, 21) }, g_ds = { CRYPTO(15, 17) }, g_hmac = { CRYPTO(17, 19) },
        g_pcnt = { { { 0x1C, BIT(30) } }, 0xC4, BIT(30) },
        g_ledc = { { { 0x20, BIT(0) }, { 0x9C, BIT(2) } }, 0xC4, BIT(29) },
        g_rmt = { { { 0x1C, BIT(0) }, { 0x9C, BIT(5) } }, 0xC4, BIT(23) },
        g_i2c0 = { { { 0x1C, BIT(12) }, { 0x40, BIT(1) } }, 0xC4, BIT(22) },
        g_i2c1 = { { { 0x1C, BIT(13) }, { 0x40, BIT(27) } }, 0xC4, BIT(21) },
        g_i2s0 = { { { 0x1C, BIT(14) } }, 0xC8, BIT(4) }, g_i2s1 = { { { 0x1C, BIT(15) } }, 0xC8, BIT(5) },
        g_twai0 = { { { 0x1C, BIT(24) }, { 0x7C, BIT(25) } }, 0, 0 }, g_twai1 = { { { 0x1C, BIT(25) }, { 0x7C, BIT(27) } }, 0, 0 },
        g_mcpwm0 = { { { 0x1C, BIT(27) }, { 0x94, BIT(2) } }, 0, 0 }, g_mcpwm1 = { { { 0x1C, BIT(28) }, { 0x94, BIT(13) } }, 0, 0 },
        g_spi2 = { { { 0x18, BIT(0) }, { 0x1C, BIT(19) }, { 0x80, BIT(3) | BIT(20) } }, 0xC8, BIT(7) },
        g_spi3 = { { { 0x18, BIT(1) }, { 0x1C, BIT(20) }, { 0x80, BIT(24) }, { 0x84, BIT(16) } }, 0xC8, BIT(8) },
        g_uhci = { { { 0x18, BIT(17) }, { 0x1C, BIT(6) } }, 0xC4, BIT(18) },
        g_adc = { { { 0x1C, BIT(5) }, { 0xA0, BIT(0) } }, 0xC8, BIT(10) },
        g_lcdcam = { { { 0x20, BIT(1) }, { 0x64, BIT(31) }, { 0x8C, BIT(29) } }, 0xC8, BIT(9) };
    switch (p) {
    case PERIPH_UART0: return &g_uart0;
    case PERIPH_UART1: return &g_uart1;
    case PERIPH_TIMG0: return &g_timg0;
    case PERIPH_TIMG1: return &g_timg1;
    case PERIPH_SYSTIMER: return &g_stimer;
    case PERIPH_AES: return &g_aes;
    case PERIPH_SHA: return &g_sha;
    case PERIPH_RSA: return &g_rsa;
    case PERIPH_DS: return &g_ds;
    case PERIPH_HMAC: return &g_hmac;
    case PERIPH_PCNT: return &g_pcnt;
    case PERIPH_LEDC: return &g_ledc;
    case PERIPH_RMT: return &g_rmt;
    case PERIPH_I2C0: return &g_i2c0;
    case PERIPH_I2C1: return &g_i2c1;
    case PERIPH_I2S0: return &g_i2s0;
    case PERIPH_I2S1: return &g_i2s1;
    case PERIPH_TWAI: return &g_twai0;
    case PERIPH_MCPWM0: return &g_mcpwm0;
    case PERIPH_MCPWM1: return &g_mcpwm1;
    case PERIPH_SPI2: return &g_spi2;
    case PERIPH_SPI3: return &g_spi3;
    case PERIPH_UHCI0: return &g_uhci;
    case PERIPH_SARADC: return &g_adc;
    case PERIPH_LCD_CAM: return &g_lcdcam;
    default: return 0;
    }
}

void periph_enable(periph_t p)
{
    const gate_t *g = gate(p);
    if (!g) return;
    uint32_t t = irq_lock();
    for (int i = 0; i < 4; i++) if (g->en[i].reg) REG_SET(HPC(g->en[i].reg), g->en[i].mask);
    if (g->rst_reg) REG_CLR(HPC(g->rst_reg), g->rst);
    irq_unlock(t);
}

void periph_disable(periph_t p)
{
    const gate_t *g = gate(p);
    if (!g) return;
    uint32_t t = irq_lock();
    if (g->rst_reg) REG_SET(HPC(g->rst_reg), g->rst);
    for (int i = 3; i >= 0; i--) if (g->en[i].reg) REG_CLR(HPC(g->en[i].reg), g->en[i].mask);
    irq_unlock(t);
}

void periph_reset(periph_t p)
{
    const gate_t *g = gate(p);
    if (!g || !g->rst_reg) return;
    uint32_t t = irq_lock();
    REG_SET(HPC(g->rst_reg), g->rst);
    REG_CLR(HPC(g->rst_reg), g->rst);
    irq_unlock(t);
}

/* Clock output (IDF esp_clock_output.c, clk_ll_bind_output_channel / _enable_output_channel / _set_output_channel_divider):
 * HP_CLKRST DBG_CLK_CTRL0/1 select the source and enable a channel, GPIO matrix signals 184..186 (DBG_CH0..2_CLK) carry it
 * to any pad. ch 1..3, src a CLK_OUT_* (clk.h) or -1 to switch the channel off. The USB pads 24 / 25 are refused. */
int clk_out(int ch, int src, int pin)
{
    static const uint8_t sel_s[3] = { 0, 8, 16 }, div_s[3] = { 24, 0, 8 }, en_s[3] = { 16, 17, 18 };
    static const uint8_t sig[3] = { 184, 185, 186 };
    if (ch < 1 || ch > 3 || pin == 24 || pin == 25) return -1;
    int c = ch - 1;
    if (src < 0) { REG_CLR(HPC(0xE8), BIT(en_s[c])); return 0; }
    int id = src == CLK_OUT_PLL ? 2 : src == CLK_OUT_XTAL ? 3 : src == CLK_OUT_RC_FAST ? 4 : src == CLK_OUT_RC_SLOW ? 5 : -1;   /* soc_clkout_sig_id_t: CPLL, XTAL, RC_FAST, RC_SLOW */
    if (id < 0 || pin < 0 || pin > 54) return -1;
    REG_FIELD(HPC(0xE4), sel_s[c], 8, id);
    if (c == 0) REG_FIELD(HPC(0xE4), div_s[0], 8, 0); else REG_FIELD(HPC(0xE8), div_s[c], 8, 0);      /* divider 1 */
    REG_SET(HPC(0xE8), BIT(en_s[c]));
    gpio_out_signal(pin, sig[c], false);
    return 0;
}
