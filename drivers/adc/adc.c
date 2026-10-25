/* SAR ADC through the RTC (SENS) controller, software-started one-shot.
 * Calibration: eFuse init code -> analog front end (regi2c), then curve
 * fitting raw -> mV. The math mirrors ESP-IDF's curve-fitting scheme so both
 * stacks produce identical millivolts from identical raw codes. */
#include "adc.h"
#include "efuse.h"
#include "regi2c.h"
#include "clk.h"
#include "gpio.h"

#define SENS_READER1   0x60008800   /* b7:0 clk div b28 data_inv */
#define SENS_MEAS1     0x6000880C   /* b15:0 data b16 done b17 start b18 start_force b30:19 en_pad b31 en_pad_force */
#define SENS_MEAS1_MUX 0x60008810   /* b31 dig_force */
#define SENS_ATTEN1    0x60008814
#define SENS_READER2   0x60008824
#define SENS_MEAS2     0x60008830
#define SENS_MEAS2_MUX 0x60008834   /* b31:28 pwdet cct */
#define SENS_ATTEN2    0x60008838
#define SENS_SLAVE1    0x60008840   /* b29:22 meas_status */
#define SENS_XPD_SAR   0x6000883C   /* b30:29 force_xpd_sar */
#define SENS_CLK_GATE  0x60008904   /* b30 saradc_clk_en */
#define ANA_CONFIG     0x6000E044
#define ANA_CONFIG2    0x6000E048
#define REGI2C_SAR     0x69

static uint8_t atten_of[2][10];
static int8_t  code_atten[2] = { -1, -1 };     /* atten whose init code is loaded */
static bool    up;
static int     cal_ver = -1;

bool adc_calibrated(void)
{
    if (cal_ver < 0) cal_ver = efuse_bits(2, 128, 2) == 1;     /* BLK_VERSION_MAJOR == 1 */
    return cal_ver == 1;
}

void adc_power_up(void)
{
    if (up) return;
    periph_enable(PERIPH_SARADC);
    REG_SET(SENS_CLK_GATE, BIT(30));
    REG_FIELD(SENS_XPD_SAR, 29, 2, 3);                        /* force SAR power on */
    REG_CLR(ANA_CONFIG, BIT(18));                             /* SAR analog i2c out of power-down */
    REG_SET(ANA_CONFIG2, BIT(16));
    REG_FIELD(SENS_READER1, 0, 8, 1);                          /* sar clk div, as IDF */
    REG_FIELD(SENS_READER2, 0, 8, 1);
    REG_CLR(SENS_READER1, BIT(28)); REG_CLR(SENS_READER2, BIT(28));
    REG_FIELD(SENS_MEAS2_MUX, 28, 3, 4);                      /* pwdet cct default */
    REG_CLR(SENS_MEAS1_MUX, BIT(31));                          /* RTC controller, not digital */
    REG_SET(SENS_MEAS1, BIT(18) | BIT(31));                    /* software start + pad map */
    REG_SET(SENS_MEAS2, BIT(18) | BIT(31));
    up = true;
}

/* eFuse init code for (unit, atten), decoded as in esp_efuse_rtc_calib.c */
static uint32_t init_code(int unit, int atten)
{
    static const uint8_t p1[4] = { 149, 157, 163, 169 }, w1[4] = { 8, 6, 6, 6 };
    static const uint8_t p2[4] = { 175, 183, 189, 195 }, w2[4] = { 8, 6, 6, 6 };
    uint32_t d[4], c[4];
    for (int i = 0; i < 4; i++) d[i] = unit == 1 ? efuse_bits(2, p1[i], w1[i]) : efuse_bits(2, p2[i], w2[i]);
    if (unit == 1) {
        c[0] = d[0] + 1850; c[1] = d[1] + c[0] + 90; c[2] = d[2] + c[1]; c[3] = d[3] + c[2] + 70;
    } else {
        c[0] = d[0] + 2020; c[1] = d[1] + c[0]; c[2] = d[2] + c[1]; c[3] = d[3] + c[2];
    }
    return c[atten];
}

void adc_load_code(int unit, int atten)
{
    if (code_atten[unit - 1] == atten) return;
    uint32_t code = adc_calibrated() ? init_code(unit, atten) : 0;
    regi2c_write_mask(REGI2C_SAR, unit == 1 ? 2 : 5, 6, 4, 4);              /* DREF = 4 */
    regi2c_write_mask(REGI2C_SAR, unit == 1 ? 1 : 4, 3, 0, code >> 8);      /* init code high */
    regi2c_write_mask(REGI2C_SAR, unit == 1 ? 0 : 3, 7, 0, code & 0xFF);    /* init code low */
    code_atten[unit - 1] = atten;
}

int adc_channel(int unit, int ch, adc_atten_t atten)
{
    if (unit < 1 || unit > 2 || ch < 0 || ch > 9 || atten > 3) return -1;
    adc_power_up();
    int pin = unit == 1 ? ch + 1 : ch + 11;
    REG(IO_MUX_GPIO(pin)) = 1u << 12;                          /* no IE, no pulls, no output */
    REG(pin < 32 ? GPIO_BASE + 0x28 : GPIO_BASE + 0x34) = BIT(pin & 31);   /* output disable */
    REG_FIELD(unit == 1 ? SENS_ATTEN1 : SENS_ATTEN2, ch * 2, 2, atten);
    atten_of[unit - 1][ch] = atten;
    return 0;
}

RAM_FN int adc_read_raw(int unit, int ch)
{
    if (!up || unit < 1 || unit > 2) return -1;
    adc_load_code(unit, atten_of[unit - 1][ch]);
    uint32_t meas = unit == 1 ? SENS_MEAS1 : SENS_MEAS2;
    if (unit == 1) REG_CLR(SENS_MEAS1_MUX, BIT(31));          /* RTC controller (continuous mode may own ADC1) */
    if (unit == 1) while ((REG(SENS_SLAVE1) >> 22) & 0xFF) {}
    REG_FIELD(meas, 19, 12, 1u << ch);
    REG_CLR(meas, BIT(17));
    REG_SET(meas, BIT(17));
    while (!(REG(meas) & BIT(16))) {}
    return REG(meas) & 0xFFFF;
}

/* ---- curve fitting (ESP-IDF scheme, integer-identical) --------------------- */
typedef struct { uint64_t c, d; } frac_t;
static const frac_t k1[4][5] = {
    { { 27856531419538344ull, 10000000000000000ull }, { 50871540569528ull, 10000000000000000ull }, { 9798249589ull, 1000000000000000ull } },
    { { 29831022915028695ull, 10000000000000000ull }, { 49393185868806ull, 10000000000000000ull }, { 101379430548ull, 10000000000000000ull } },
    { { 23285545746296417ull, 10000000000000000ull }, { 147640181047414ull, 10000000000000000ull }, { 208385525314ull, 10000000000000000ull } },
    { { 644403418269478ull, 1000000000000000ull }, { 644334888647536ull, 10000000000000000ull }, { 1297891447611ull, 10000000000000000ull },
      { 70769718ull, 1000000000000000ull }, { 13515ull, 1000000000000000ull } },
};
static const frac_t k2[4][5] = {
    { { 25668651654328927ull, 10000000000000000ull }, { 1353548869615ull, 10000000000000000ull }, { 36615265189ull, 10000000000000000ull } },
    { { 23690184690298404ull, 10000000000000000ull }, { 66319894226185ull, 10000000000000000ull }, { 118964995959ull, 10000000000000000ull } },
    { { 9452499397020617ull, 10000000000000000ull }, { 200996773954387ull, 10000000000000000ull }, { 259011467956ull, 10000000000000000ull } },
    { { 12247719764336924ull, 10000000000000000ull }, { 755717904943462ull, 10000000000000000ull }, { 1478791187119ull, 10000000000000000ull },
      { 79672528ull, 1000000000000000ull }, { 15038ull, 1000000000000000ull } },
};
static const int8_t s1[4][5] = { { -1, -1, 1 }, { -1, -1, 1 }, { -1, -1, 1 }, { -1, -1, 1, -1, 1 } };
static const int8_t s2[4][5] = { { -1, 1, 1 }, { -1, -1, 1 }, { -1, -1, 1 }, { 1, -1, 1, -1, 1 } };

static uint32_t cal_digi(int unit, int atten)       /* eFuse reference reading at 850 mV */
{
    static const uint8_t pos[8] = { 201, 209, 217, 225, 233, 241, 248, 186 }, w[8] = { 8, 8, 8, 8, 8, 7, 7, 6 };
    uint32_t d[8] = { 0 }, v1[4], v2[4];
    if (atten < 0 || atten > 3) return 0;                           /* caller data: no index past the tables */
    for (int i = 0; i < 8; i++) d[i] = efuse_bits(i == 7 ? 1 : 2, pos[i], w[i]);
    v1[3] = d[3] + 900; v1[2] = d[2] + v1[3] + 800; v1[1] = d[1] + v1[2] + 700; v1[0] = d[0] + v1[1] + 800;
    v2[3] = v1[3] - d[7] + 15; v2[2] = v1[2] - d[6] + 20; v2[1] = v1[1] - d[5] + 10; v2[0] = v1[0] - d[4] + 40;
    return unit == 1 ? v1[atten] : v2[atten];
}

int adc_raw_to_mv(int unit, adc_atten_t atten, int raw)
{
    if (!adc_calibrated() || raw < 0 || (unit != 1 && unit != 2) || (unsigned)atten > 3) return -1;
    uint32_t cd = cal_digi(unit, atten);
    if (!cd) return -1;
    int32_t coeff_a = 65536 * 850 / cd;
    uint64_t v = (uint64_t)raw * coeff_a / 65536;
    const frac_t *k = unit == 1 ? k1[atten] : k2[atten];
    const int8_t *s = unit == 1 ? s1[atten] : s2[atten];
    int terms = atten == 3 ? 5 : 3;
    int32_t err = 0;
    if (v) {
        uint64_t x = 1;
        err = (int32_t)(k[0].c / k[0].d) * s[0];
        for (int i = 1; i < terms; i++) {
            x *= v;
            err += (int32_t)(x * k[i].c / k[i].d) * s[i];
        }
    }
    return (int32_t)v - err;
}

int adc_read_mv(int unit, int ch)
{
    return adc_raw_to_mv(unit, atten_of[unit - 1][ch], adc_read_raw(unit, ch));
}
