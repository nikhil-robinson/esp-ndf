/* P4 temperature sensor, same API as drivers/tsens/tsens.c. The P4 block is the LP_TSENS peripheral (0x5012F000); the
 * conversion is the S3's (IDF temperature_sensor_ll.h: out = (T + 20.52 + 27.88 * offset) / 0.4386, range DAC at
 * regi2c SAR_ADC 0x69 reg 6 bits 3:0, 15 = -10..80 C, 7 = 20..100 C) and the per-chip trim is eFuse
 * SYS_DATA_PART2 TEMPERATURE_SENSOR (sign bit 8). Sampling is continuous (ctrl.sample_en) and polled. */
#include "tsens.h"
#include "soc.h"
#include "sys.h"
#include "regi2c.h"
#include "cpu.h"
#include "efuse.h"

#define TSENS_BASE    (LPPERIPH + 0xF000)
#define TSENS_CTRL    (TSENS_BASE + 0x0)    /* b7:0 out b8 ready b9 sample_en b21:14 clk_div b22 power_up b23 power_up_force */
#define LPPERI_CLK_EN (LPPERIPH + 0x0)      /* b17 LP_TSENS clock */
#define LPPERI_RST_EN (LPPERIPH + 0x8)      /* b18 LP_TSENS reset (LPPERI_RESET_EN) */

static float trim;
static int hi_range;

static uint32_t raw_read(void)
{
    uint32_t t = ccount(), lim = 10000u * (sys_cpu_hz() / 1000000u);
    while (!(REG(TSENS_CTRL) & BIT(8)) && ccount() - t < lim) {}
    return REG(TSENS_CTRL) & 0xFF;
}

void tsens_init(void)
{
    REG_SET(LPPERI_CLK_EN, BIT(17));
    REG_SET(LPPERI_RST_EN, BIT(18)); REG_CLR(LPPERI_RST_EN, BIT(18));
    regi2c_write_mask(0x69, 0x06, 3, 0, 15);
    REG_SET(TSENS_CTRL, BIT(22) | BIT(23));          /* power up (forced) */
    REG_SET(TSENS_CTRL, BIT(9));                      /* sampling on */
    delay_us(300);
    hi_range = 0;
    raw_read();
    uint32_t t = efuse_get("TEMPERATURE_SENSOR");
    trim = (t & BIT(8) ? -(float)(t & 0xFF) : (float)(t & 0xFF)) / 10.0f;
}

static void set_range(int hi)
{
    regi2c_write_mask(0x69, 0x06, 3, 0, hi ? 7 : 15);
    hi_range = hi;
    delay_us(1000);
    raw_read();
}

float tsens_read_c(void)
{
    float c = 0.4386f * (float)raw_read() - 20.52f + (hi_range ? 27.88f : 0);
    if (!hi_range && c > 75.0f) { set_range(1); c = 0.4386f * (float)raw_read() - 20.52f + 27.88f; }
    else if (hi_range && c < 25.0f) { set_range(0); c = 0.4386f * (float)raw_read() - 20.52f; }
    return c - trim;
}
