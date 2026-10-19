#include "rf_init.h"
#include "rf_init_replay_c948.h"
#include "rf_regi2c.h"

#define REG(a) (*(volatile uint32_t *)(a))
void rf_delay_us(uint32_t us);                  /* platform hook */

#define RTC_DIG_PWC   0x60008090u               /* bit 17 WIFI_FORCE_PD */
#define RTC_DIG_ISO   0x60008094u               /* bit 28 WIFI_FORCE_ISO */
#define SYS_WIFI_CLK  0x60026014u
#define SYS_WIFI_RST  0x60026018u
#define MODEM_RST     0x2A1Fu                   /* BB, FE, MAC, BT BB/MAC (+regs) */
#define MODEM_CLK     0x00788F8Fu               /* common modem clocks + calibration clock (bit 22) */
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
