/* ESP32-S3 clock tree: BBPLL bring-up, CPU frequency, peripheral gates.
 *   XTAL 40 MHz --> BBPLL 480 MHz --> CPU 240/160/80 (APB fixed 80 MHz)
 *               \-> CPU = XTAL 40 MHz (APB 40 MHz) */
#include "clk.h"
#include "soc.h"
#include "regi2c.h"
#include "xtensa.h"

#define SYSTEM_CPU_PERI_CLK  (SYSTEM_BASE + 0x008)
#define SYSTEM_CPU_PERI_RST  (SYSTEM_BASE + 0x00C)
#define SYSTEM_PERIP_CLK1    (SYSTEM_BASE + 0x01C)
#define SYSTEM_PERIP_RST1    (SYSTEM_BASE + 0x024)

static uint32_t cpu_hz = 20000000;            /* ROM leaves XTAL/2 */

static const uint32_t clk_reg[3] = { SYSTEM_PERIP_CLK0, SYSTEM_PERIP_CLK1, SYSTEM_CPU_PERI_CLK };
static const uint32_t rst_reg[3] = { SYSTEM_PERIP_RST0, SYSTEM_PERIP_RST1, SYSTEM_CPU_PERI_RST };

void periph_enable(periph_t p)
{
    uint32_t ps = irq_lock();
    REG_SET(clk_reg[p >> 8], BIT(p & 31));
    REG_CLR(rst_reg[p >> 8], BIT(p & 31));
    irq_unlock(ps);
}

void periph_disable(periph_t p)
{
    uint32_t ps = irq_lock();
    REG_SET(rst_reg[p >> 8], BIT(p & 31));
    REG_CLR(clk_reg[p >> 8], BIT(p & 31));
    irq_unlock(ps);
}

void periph_reset(periph_t p)
{
    uint32_t ps = irq_lock();
    REG_SET(rst_reg[p >> 8], BIT(p & 31));
    REG_CLR(rst_reg[p >> 8], BIT(p & 31));
    irq_unlock(ps);
}

uint32_t clk_cpu_hz(void) { return cpu_hz; }
uint32_t clk_cpu_sync(void)                    /* cpu_hz from the hardware (a loader may have set the clock) */
{
    uint32_t sel = (REG(SYSTEM_SYSCLK_CONF) >> 10) & 3;
    if (sel == 1) cpu_hz = (REG(SYSTEM_CPU_PER) & 3) == 2 ? 240000000u : (REG(SYSTEM_CPU_PER) & 3) == 1 ? 160000000u : 80000000u;
    else if (sel == 0) cpu_hz = 40000000u / ((REG(SYSTEM_SYSCLK_CONF) & 0x3FF) + 1);
    return cpu_hz;
}
uint32_t clk_apb_hz(void) { return cpu_hz >= 80000000 ? 80000000 : cpu_hz; }

static void spin_us(uint32_t us)           /* cpu_hz must be current */
{
    uint32_t t = ccount(), n = us * (cpu_hz / 1000000);
    while (ccount() - t < n) {}
}

/* Core voltage: 28 for 240 MHz, 27 below (esp-idf untrimmed defaults).
 * ponytail: no eFuse PVT trim; add with the eFuse driver if a chip is marginal. */
static void dig_dbias(int v, bool settle)
{
    regi2c_write_mask(REGI2C_DIG, 4, 4, 0, v);    /* EXT_RTC_DREG */
    regi2c_write_mask(REGI2C_DIG, 6, 4, 0, v);    /* EXT_DIG_DREG */
    if (settle) spin_us(40);                      /* raising: settle before the clock goes up; lowering: the clock
                                                   * is already down, nothing waits on the voltage */
}

void clk_bbpll_init(void)
{
    REG_FIELD(SYSTEM_SYSCLK_CONF, 10, 2, 0);              /* CPU on XTAL meanwhile */
    REG_FIELD(SYSTEM_SYSCLK_CONF, 0, 10, 0);              /* PRE_DIV /1: 40 MHz    */
    cpu_hz = CLK_XTAL_HZ;
    REG_CLR(RTC_OPTIONS0, BIT(6) | BIT(8) | BIT(10));    /* BBPLL + its i2c powered */
    REG_FIELD(SYSTEM_CPU_PER, 2, 1, 1);                  /* PLL 480 MHz */

    REG_CLR(I2C_ANA_CONF0, BIT(2)); REG_SET(I2C_ANA_CONF0, BIT(3));   /* cal start */
    regi2c_write(REGI2C_BBPLL, 4, 0x6B);                 /* MODE_HF                */
    regi2c_write(REGI2C_BBPLL, 2, 5 << 4 | 0);           /* DCHGP=5, REF_DIV=0     */
    regi2c_write(REGI2C_BBPLL, 3, 8);                    /* DIV                    */
    regi2c_write_mask(REGI2C_BBPLL, 5, 2, 0, 0);         /* DR1                    */
    regi2c_write_mask(REGI2C_BBPLL, 5, 6, 4, 0);         /* DR3                    */
    regi2c_write(REGI2C_BBPLL, 6, 1 << 6 | 3 << 4 | 3);  /* DLREF, DHREF, DCUR     */
    regi2c_write_mask(REGI2C_BBPLL, 9, 1, 0, 3);         /* VCO_DBIAS              */
    while (!(REG(I2C_ANA_CONF0) & BIT(24))) {}           /* cal done               */
    spin_us(10);
    REG_CLR(I2C_ANA_CONF0, BIT(3)); REG_SET(I2C_ANA_CONF0, BIT(2));   /* cal stop  */
}

int clk_cpu_set_mhz(uint32_t mhz)
{
    int sel;
    switch (mhz) {
    case 240: sel = 2; break;
    case 160: sel = 1; break;
    case 80:  sel = 0; break;
    case 40:  sel = -1; break;
    default:  return -1;
    }
    uint32_t ps = irq_lock();
    bool up = mhz * 1000000 > cpu_hz;
    if (up && mhz == 240) dig_dbias(28, true);
    if (up) REG_FIELD(RTC_DATE, 13, 6, 0x7 >> (mhz / 80));     /* LDO slaves first */
    if (sel < 0) {
        REG_FIELD(SYSTEM_SYSCLK_CONF, 10, 2, 0);                 /* XTAL /1 */
    } else {
        REG_FIELD(SYSTEM_CPU_PER, 0, 2, sel);
        REG_FIELD(SYSTEM_SYSCLK_CONF, 10, 2, 1);                 /* PLL */
    }
    bool was240 = cpu_hz == 240000000;
    cpu_hz = mhz * 1000000;
    if (!up) REG_FIELD(RTC_DATE, 13, 6, 0x7 >> (mhz / 80));
    if (!up && was240) dig_dbias(27, false);
    irq_unlock(ps);
    return 0;
}

/* Clock output (IDF esp_clock_output): IO_MUX PIN_CTRL selects the source per channel, the pad takes the CLK_OUTn
 * function. Channel pads: 1 = GPIO41 (MTDI) / 43, 2 = GPIO40 (MTDO) / 44, 3 = GPIO39 (MTCK) / 18; GPIO19/20 (also
 * CLK_OUT2/1) are the USB pins and are refused. src = CLK_OUT_* (TRM / clk_tree_defs.h ids). */
int clk_out(int ch, int src, int pin)
{
    static const struct { int8_t pin, fn; } pads[3][2] = { { { 41, 2 }, { 43, 2 } }, { { 40, 2 }, { 44, 2 } }, { { 39, 2 }, { 18, 3 } } };
    if (ch < 1 || ch > 3) return -1;
    int fn = -1;
    for (int i = 0; i < 2; i++) if (pads[ch - 1][i].pin == pin) fn = pads[ch - 1][i].fn;
    if (fn < 0) return -1;
    REG_FIELD(0x60009000u, 4 * (ch - 1), 4, src < 0 ? 0xF : src);   /* PIN_CTRL CLK_OUTn (0xF = off, reset value) */
    if (src < 0) return 0;
    if (src == CLK_OUT_RC_FAST) REG_SET(0x60008074u, BIT(10));     /* RTC_CNTL_CLK_CONF dig_clk8m_en, as IDF */
    REG_FIELD(IO_MUX_GPIO(pin), 12, 3, fn);                          /* MCU_SEL = CLK_OUTn */
    REG_SET(IO_MUX_GPIO(pin), BIT(9));                               /* input on: readable back */
    return 0;
}
