/* Temperature sensor: SENS block (0x60008800) + range DAC on the analog bus
 * (block 0x69 reg 6 bits 3:0). Sequence and conversion after the IDF HAL
 * (temperature_sensor_ll.h): out = (T + 20.52 + 27.88 * offset) / 0.4386,
 * range "offset 0" = DAC 15 (-10..80 C), "offset -1" = DAC 7 (20..100 C), chosen automatically. Per-chip trim from
 * eFuse TEMP_CALIB (sign bit 8, 0.1 C units) when BLK_VERSION_MAJOR = 1, as IDF esp_efuse_rtc_calib_get_tsens_val. */
#include "tsens.h"
#include "soc.h"
#include "sys.h"
#include "regi2c.h"
#include "cpu.h"
#include "efuse.h"

static float trim;
static int hi_range;
static uint32_t raw_read(void);

#define SENS_TCTRL   0x60008850u                /* 24 dump_out, 23 power_up_force, 22 power_up, 8 ready, 7:0 out */
#define SENS_TCTRL2  0x60008854u                /* 12 xpd_force */
#define SENS_CLKGATE 0x60008904u                /* 29 tsens clock */
#define SENS_RESET   0x60008908u                /* 29 tsens reset */

void tsens_init(void)
{
    REG(0x60008074u) |= BIT(10);                /* RTC_CNTL_CLK_CONF dig_clk8m_en: IDF's default tsens clock is RC_FAST */
    delay_us(5);
    REG(SENS_CLKGATE) |= BIT(29);
    REG(SENS_RESET) |= BIT(29);
    REG(SENS_RESET) &= ~BIT(29);
    REG(SENS_TCTRL) |= BIT(23) | BIT(22);
    REG(SENS_TCTRL2) |= BIT(12);
    regi2c_write_mask(0x69, 0x06, 3, 0, 15);
    delay_us(300);                              /* sensor settle after power-up */
    hi_range = 0;
    raw_read();                                 /* first conversion after power-up is stale */
    trim = 0;
    if (efuse_get("BLK_VERSION_MAJOR") == 1) {
        uint32_t t = efuse_get("TEMP_CALIB");
        trim = (t & BIT(8) ? -(float)(t & 0xFF) : (float)(t & 0xFF)) / 10.0f;
    }
}

static uint32_t raw_read(void)
{
    REG(SENS_TCTRL) |= BIT(24);
    uint32_t t = ccount(), lim = 10000u * (sys_cpu_hz() / 1000000u);
    while (!(REG(SENS_TCTRL) & BIT(8)) && ccount() - t < lim) {}
    uint32_t raw = REG(SENS_TCTRL) & 0xFF;
    REG(SENS_TCTRL) &= ~BIT(24);
    return raw;
}

/* range table (IDF temperature_sensor_periph.c): offset -1 = DAC 7 (20..100 C), 0 = DAC 15 (-10..80 C).
 * The range stays selected; it changes only when a reading leaves it (first conversion after a change is stale). */

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
