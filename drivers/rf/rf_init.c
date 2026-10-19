#include "rf_init.h"
#include "rf_init_replay_c948.h"
#include "rf_txcal_c948.h"
#include "rf_regi2c.h"

#define REG(a) (*(volatile uint32_t *)(a))
void rf_delay_us(uint32_t us);                  /* platform hook */

#define RTC_DIG_PWC   0x60008090u               /* bit 17 WIFI_FORCE_PD */
#define RTC_DIG_ISO   0x60008094u               /* bit 28 WIFI_FORCE_ISO */
#define SYS_WIFI_CLK  0x60026014u
#define SYS_WIFI_RST  0x60026018u
#define MODEM_RST     0x2A1Fu                   /* BB, FE, MAC, BT BB/MAC (+regs) */
#define MODEM_CLK     0x00FB9FCFu               /* all modem clocks (IDF SYSTEM_WIFI_CLK_EN); bits 6/12/16/17 feed the dump engine path */
#define PBUS_CMD      0x60006104u
#define PBUS_STAT     0x60006110u               /* bit 31 busy; bits 15:14 RX clock */
#define PBUS_MODE     0x6000610Cu
#define GAIN_CMD      0x600060C8u
#define GAIN_DATA     0x600060CCu
#define AGC_GAIN      0x6001C02Cu               /* 31:24 gain, 23 force */
#define WIFI_BB_CFG   0x6002600Cu               /* bit 1: baseband RX path running (Wi-Fi start sets it, stop clears it);
                                                   without it the dump engine records one frozen sample */

void rf_power_on(void)
{
    REG(SYS_WIFI_CLK) |= MODEM_CLK;
    REG(RTC_DIG_PWC) &= ~(1u << 17);
    rf_delay_us(10);
    REG(SYS_WIFI_RST) |= MODEM_RST;
    REG(SYS_WIFI_RST) &= ~MODEM_RST;
    REG(RTC_DIG_ISO) &= ~(1u << 28);
}

void rf_pbus_write(uint8_t row, uint8_t col, uint16_t value)
{
    uint32_t f = ((uint32_t)value << 6 | (uint32_t)row << 2 | (uint32_t)col << 15) & 0x1FFFC;
    REG(PBUS_CMD) = (REG(PBUS_CMD) & 0xFFFE0001u) | f | 2;
    while (REG(PBUS_STAT) & 0x80000000u) {}
    REG(PBUS_CMD) &= ~2u;
}

/* rom_pbus_force_mode: leaving force mode also pulses the AGC gain to 0x32
 * (forced for 2 us) when 0x6002600C bit 1 is set. */
void rf_pbus_force_mode(int on)
{
    if (on) {
        if (!(REG(WIFI_BB_CFG) & 2)) REG(PBUS_MODE) &= ~(1u << 27);
        REG(PBUS_CMD) |= 1;
        return;
    }
    REG(PBUS_CMD) &= ~1u;
    REG(PBUS_MODE) |= 1u << 27;
    if (REG(WIFI_BB_CFG) & 2) {
        rf_delay_us(1);
        REG(AGC_GAIN) = (REG(AGC_GAIN) & 0x00FFFFFFu) | 0x32000000u;
        REG(AGC_GAIN) |= 1u << 23;
        rf_delay_us(2);
        REG(AGC_GAIN) &= ~(1u << 23);
    }
}

void rf_pbus_xpd_tx_off(void)
{
    rf_pbus_write(4, 1, 0); rf_pbus_write(5, 1, 0); rf_pbus_write(1, 1, 0);
    rf_pbus_write(1, 2, 0); rf_pbus_write(0, 1, 0);
}

void rf_pbus_xpd_rx_on(uint16_t rxgain)
{
    rf_pbus_write(4, 1, 0); rf_pbus_write(5, 1, 0); rf_pbus_write(0, 1, 0x184);
    rf_pbus_write(1, 1, 0x189); rf_pbus_write(1, 2, rxgain);
}

void rf_gain_mem_write(uint8_t index, uint32_t lo, uint32_t hi)
{
    REG(GAIN_DATA) = hi; REG(GAIN_CMD) = (uint32_t)index << 8 | 0x100F0;
    REG(GAIN_DATA) = lo; REG(GAIN_CMD) = (uint32_t)index << 8 | 0x1000F;
}

static void apply_digital(void)
{
    for (unsigned i = 0; i < sizeof rf_init_digital / sizeof rf_init_digital[0]; i++)
        REG(rf_init_digital[i].addr) = rf_init_digital[i].value;
}

/* ponytail: replays one chip's traced init (calibration results included);
 * replace op groups with our own calibrations one by one. */
void rf_init_replay(void)
{
    rf_power_on();
    apply_digital();
    for (unsigned i = 0; i < sizeof rf_init_ops / sizeof rf_init_ops[0]; i++) {
        const rf_op_t *o = &rf_init_ops[i];
        switch (o->op) {
        case 1:
            if (o->c == 0x70) rf_regi2c_write(o->a, o->b, (uint8_t)o->x);
            else rf_regi2c_write_bits(o->a, o->b, o->c >> 4, o->c & 7, (uint8_t)o->x);
            break;
        case 2: rf_pbus_write(o->a, o->b, (uint16_t)o->x); break;
        case 3: rf_gain_mem_write(o->a, o->x, o->y); break;
        case 4: rf_pbus_force_mode(o->a); break;
        }
    }
    apply_digital();                            /* final values of words the steps rewrote */
}

void rf_rx_on(void)
{
    REG(WIFI_BB_CFG) |= 2;
    rf_pbus_force_mode(0);
    rf_pbus_xpd_tx_off();
    rf_pbus_xpd_rx_on(1);
    REG(PBUS_STAT) |= 3u << 14;                 /* RX clock */
    rf_delay_us(100);
}

void rf_rx_gain(int gain)
{
    if (gain < 0) { REG(AGC_GAIN) &= ~(1u << 23); return; }
    REG(AGC_GAIN) = (REG(AGC_GAIN) & 0x007FFFFFu) | (uint32_t)gain << 24 | 1u << 23;
}

#define FE_CTRL   0x60006000u                   /* 1 TX forced on, 17:10 forced TX row, 25:18 TX row base, 26 tone path off */
#define TONE1     0x60006040u                   /* bit 18 tone on */

void rf_tone_off(void)
{
    REG(TONE1) &= ~(1u << 18);
    REG(FE_CTRL) |= 1u << 26;
    REG(0x600061E4u) &= ~(1u << 10);
}

void rf_pbus_xpd_tx_on(uint16_t pa, uint16_t mix)
{
    rf_pbus_write(2, 1, 0x100); rf_pbus_write(3, 1, 0x100);   /* DCO words */
    rf_pbus_write(2, 2, 0x100); rf_pbus_write(3, 2, 0x100);
    rf_pbus_write(0, 1, 1); rf_pbus_write(1, 1, 124); rf_pbus_write(1, 2, mix);
    rf_pbus_write(4, 1, 127); rf_pbus_write(5, 1, pa); rf_pbus_write(4, 2, 0);
}

/* rom_set_tx_gain_mem for n rows all = (gain, bb): row words from the per-chip
 * TX calibration (cal[class][4], cal2) where class comes from the bb code. */
static void tx_rows(unsigned base, unsigned n, uint8_t gain, uint16_t bb, const uint16_t (*cal)[4], uint16_t cal2, unsigned cls)
{
    const uint16_t *w = cal[cls];
    uint32_t lo = (uint32_t)w[0] << 12 | (uint32_t)w[1] << 3 | w[2] >> 6 | (uint32_t)cal2 << 21;
    uint32_t hi = (uint32_t)w[2] << 26 | (uint32_t)w[3] << 17 | (uint32_t)gain << 10 | (uint32_t)(bb & 0x3F) << 3 | (bb >> 6 & 0xFF);
    for (unsigned i = 0; i < n; i++) rf_gain_mem_write((uint8_t)(base + i), lo, hi);
}

void rf_tx_gain(uint8_t gain, uint16_t bb, int8_t dig)
{
    unsigned base = REG(FE_CTRL) >> 18 & 0xFF;
    unsigned wcls = bb == 128 ? 1 : bb == 32 ? 3 : bb == 160 ? 4 : bb == 256 ? 2 : 0;   /* rom_txbbgain_to_index */
    unsigned bcls = bb == 128 ? 1 : bb == 256 ? 2 : 0;                                  /* rom_bt_bb_to_index */
    tx_rows(base, 14, gain, bb, rf_txcal_wifi, rf_txcal_wifi2, wcls);
    tx_rows(base + 16, 16, gain, bb, rf_txcal_bt, rf_txcal_bt2, bcls);
    uint32_t d = (uint8_t)dig * 0x01010101u;
    for (uint32_t a = 0x60006014u; a <= 0x60006030u; a += 4) REG(a) = d;           /* BT 14..20, Wi-Fi 24..30 */
}

/* force_txon_mode(on, 0, 0) then rom_force_txon(on): 0x60006110 bits 9:8 = 2
 * and 11:10 = 3 force the TX enables, both cleared to release */
void rf_txon(int on)
{
    uint32_t base = REG(FE_CTRL) >> 18 & 0xFF;
    REG(PBUS_STAT) &= ~(3u << 12);
    REG(FE_CTRL) = (REG(FE_CTRL) & ~2u) | (on ? 2u : 0);
    REG(FE_CTRL) = (REG(FE_CTRL) & 0xFFFC03FFu) | base << 10;
    if (on) {
        REG(PBUS_STAT) = (REG(PBUS_STAT) & ~(3u << 8)) | 2u << 8;
        REG(PBUS_STAT) |= 3u << 10;
    } else {
        REG(PBUS_STAT) &= ~(3u << 10);
        REG(PBUS_STAT) &= ~(3u << 8);
    }
}

void rf_tx_on(uint8_t gain, uint16_t bb)
{
    REG(WIFI_BB_CFG) |= 2;
    rf_tone_off();
    rf_pbus_force_mode(0);
    rf_pbus_xpd_rx_on(0);
    rf_pbus_xpd_tx_on(1, 0);
    rf_tx_gain(gain, bb, 0);
    rf_txon(1);
    rf_delay_us(50);
}

void rf_tx_off(void)
{
    rf_txon(0);
    rf_rx_on();
}
