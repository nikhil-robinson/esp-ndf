/* Temperature sensor: SENS block (0x60008800) + range DAC on the analog bus
 * (block 0x69 reg 6 bits 3:0). Sequence and conversion after the IDF HAL
 * (temperature_sensor_ll.h): out = (T + 20.52 + 27.88 * offset) / 0.4386,
 * range "offset 0" = DAC 15 (-10..80 C), "offset -1" = DAC 7 (20..100 C), chosen automatically. The eFuse per-chip trim is not applied. */
#include "tsens.h"
#include "soc.h"
#include "sys.h"
#include "regi2c.h"
#include "xtensa.h"

#define SENS_TCTRL   0x60008850u                /* 24 dump_out, 23 power_up_force, 22 power_up, 8 ready, 7:0 out */
#define SENS_TCTRL2  0x60008854u                /* 12 xpd_force */
#define SENS_CLKGATE 0x60008904u                /* 29 tsens clock */
#define SENS_RESET   0x60008908u                /* 29 tsens reset */

void tsens_init(void)
{
    REG(SENS_CLKGATE) |= BIT(29);
    REG(SENS_RESET) |= BIT(29);
    REG(SENS_RESET) &= ~BIT(29);
    REG(SENS_TCTRL) |= BIT(23) | BIT(22);
    REG(SENS_TCTRL2) |= BIT(12);
    regi2c_write_mask(0x69, 0x06, 3, 0, 15);
    delay_us(300);                              /* sensor settle after power-up */
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

/* range table (IDF temperature_sensor_periph.c): offset -1 = DAC 7 (20..100 C), 0 = DAC 15 (-10..80 C) */
float tsens_read_c(void)
{
    regi2c_write_mask(0x69, 0x06, 3, 0, 15);
    delay_us(1000);
    raw_read();
    uint32_t raw = raw_read();
    float c = 0.4386f * (float)raw - 20.52f;
    if (c > 75.0f) {                            /* near the top of this range: use 20..100 C */
        regi2c_write_mask(0x69, 0x06, 3, 0, 7);
        delay_us(1000);
        raw_read();                             /* first conversion after a range change is stale */
        c = 0.4386f * (float)raw_read() - 20.52f + 27.88f;
    }
    return c;
}
