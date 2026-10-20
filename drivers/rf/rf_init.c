#include "rf_init.h"
#include "rf_init_replay_c948.h"
#include "rf_txcal_c948.h"
#include "rf_pbus_mem.h"
#include "rf_regi2c.h"
#include "rf_pll.h"
#include "rf_dump.h"

#define REG(a) (*(volatile uint32_t *)(a))
void rf_delay_us(uint32_t us);                  /* platform hook */

#define RTC_ANA_CONF  0x60008034u               /* RF analog-bus power: survives a chip reset, off from cold */
#define ANA_RF_PU     (1u << 31 | 1u << 30 | 1u << 28 | 1u << 27)   /* PLL, CKGEN, RFRX pbus, TXRF i2c */
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
    REG(RTC_ANA_CONF) |= ANA_RF_PU;
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

uint32_t rf_gain_shadow[256][2];               /* what gain memory holds (it cannot be read back) */

void rf_gain_mem_write(uint8_t index, uint32_t lo, uint32_t hi)
{
    rf_gain_shadow[index][0] = lo; rf_gain_shadow[index][1] = hi;
    REG(GAIN_DATA) = hi; REG(GAIN_CMD) = (uint32_t)index << 8 | 0x100F0;
    REG(GAIN_DATA) = lo; REG(GAIN_CMD) = (uint32_t)index << 8 | 0x1000F;
}

static void apply_digital(void)
{
    for (unsigned i = 0; i < sizeof rf_init_digital / sizeof rf_init_digital[0]; i++)
        REG(rf_init_digital[i].addr) = rf_init_digital[i].value;
}

/* RX filter RC calibration (vendor init, analog 6A): measure the RC code, then
 * offset 67.14..67.1F by it. C9:48's trace measured RF_RC_REF and its replayed
 * values belong to that code; D0 measured +1 and the vendor wrote every one +1. */
#define RF_RC_REF 0x23
int rf_rc_cal(void)
{
    rf_regi2c_write_bits(0x6A, 0x02, 6, 5, 2);
    rf_regi2c_write_bits(0x6A, 0x06, 4, 0, 2);
    rf_regi2c_write_bits(0x6A, 0x04, 7, 4, 0xB);
    rf_regi2c_write_bits(0x61, 0x08, 2, 2, 1);
    rf_regi2c_write_bits(0x6A, 0x04, 0, 0, 1);
    rf_regi2c_write_bits(0x6A, 0x04, 3, 3, 0);
    rf_regi2c_write_bits(0x6A, 0x04, 3, 3, 1);
    rf_delay_us(100);                           /* settles in ~10 us, then dithers by one code */
    int sum = 0;
    for (int k = 0; k < 16; k++) { sum += rf_regi2c_read_bits(0x6A, 0x05, 5, 0); rf_delay_us(10); }
    int code = (sum + 8) / 16;
    rf_regi2c_write_bits(0x61, 0x08, 2, 2, 0);
    rf_regi2c_write_bits(0x6A, 0x04, 0, 0, 0);
    int d = code - RF_RC_REF;
    static const uint8_t regs[] = { 0x14, 0x15, 0x16, 0x17, 0x1C, 0x1D, 0x1E, 0x1F };
    for (unsigned k = 0; k < sizeof regs; k++) rf_regi2c_write(0x67, regs[k], (uint8_t)(rf_regi2c_read(0x67, regs[k]) + d));
    return code;
}

/* pbus memory (libphy set_pbus_mem, called by bb_init): per-state front-end
 * command lists the hardware applies by itself on TX/RX transitions. They sit in
 * the gain-memory port at addresses 0x200.. (data 0x600060CC, address
 * 0x600060C8[17:8], bits 17:16 cleared after each word) with one 16-bit range
 * field per state in 0x600060E0..F4. Write-only: no register dump shows it, and
 * without it the transmitter is silent from a power-on (it survives other resets). */
static void pbus_mem_load(void)
{
    for (unsigned k = 0; k < sizeof rf_pbus_mem_ranges / sizeof rf_pbus_mem_ranges[0]; k++)
        REG(rf_pbus_mem_ranges[k].a) = rf_pbus_mem_ranges[k].v;
    REG(GAIN_CMD) = (REG(GAIN_CMD) & ~0xFFu) | 0x0F;   /* lane bits as the vendor leaves them (last gain write) */
    for (unsigned k = 0; k < sizeof rf_pbus_mem / sizeof rf_pbus_mem[0]; k++) {
        REG(GAIN_DATA) = rf_pbus_mem[k];
        REG(GAIN_CMD) = (REG(GAIN_CMD) & 0xFFFC00FFu) | (uint32_t)((0x200 + k) & 0x3FF) << 8;
        REG(GAIN_CMD) &= 0xFFFCFFFFu;
    }
}

/* ponytail: replays one chip's traced init (calibration results included);
 * replace op groups with our own calibrations one by one. */
void rf_init_replay(void)
{
    rf_power_on();
    rf_regi2c_write_bits(0x67, 0x00, 6, 6, 0);  /* loopback off: analog state survives a chip reset */
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
    pbus_mem_load();
    rf_rc_cal();                                /* this chip's RX filter RC code, not the traced one */
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

static uint16_t txcal[5][4];                   /* RAM copy of rf_txcal_wifi: rf_tx_dc_cal rewrites w0/w2 */
/* TX IQ word (row lo[31:21], also 0x6000607C [10:0]): gain [4:0] 5-bit signed, phase
 * [10:5] 6-bit signed. Neutral (0) measured -43 dBc image over the air on D0, the copied
 * word -34 dBc, the vendor's own calibration -46 dBc. */
static uint16_t txcal2 = 0;
void rf_tx_set_iq(uint16_t v) { txcal2 = v & 0x7FF; }
static int txcal_init;
static unsigned wifi_class(uint16_t bb) { return bb == 128 ? 1 : bb == 32 ? 3 : bb == 160 ? 4 : bb == 256 ? 2 : 0; }   /* rom_txbbgain_to_index */

void rf_tx_gain(uint8_t gain, uint16_t bb, int8_t dig)
{
    unsigned base = REG(FE_CTRL) >> 18 & 0xFF;
    unsigned wcls = wifi_class(bb);
    unsigned bcls = bb == 128 ? 1 : bb == 256 ? 2 : 0;                                  /* rom_bt_bb_to_index */
    if (!txcal_init) { for (int r = 0; r < 5; r++) for (int k = 0; k < 4; k++) txcal[r][k] = rf_txcal_wifi[r][k]; txcal_init = 1; }
    tx_rows(base, 14, gain, bb, (const uint16_t (*)[4])txcal, txcal2, wcls);
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
    rf_rx_gain(-1);                             /* a forced RX gain left on blocks transmit (measured over the air) */
    REG(WIFI_BB_CFG) |= 2;
    rf_tone_off();
    rf_pbus_force_mode(0);
    rf_pbus_xpd_rx_on(0);
    rf_pbus_xpd_tx_on(1, 0);
    rf_tx_gain(gain, bb, 0);
    REG(0x6000607Cu) = (REG(0x6000607Cu) & ~0x7FFu) | txcal2;
    rf_txon(1);
    rf_delay_us(50);
}

void rf_tx_off(void)
{
    rf_txon(0);
    rf_rx_on();
}

/* internal TX -> RX loopback (rom_loopback_mode_en + rom_set_loopback_gain):
 * analog 67.00[6], TX and RX clocks, then PA step pa (0..15) and RX gain word
 * rxg (0x80 / 0xA0 in the vendor TX calibration) */
void rf_loopback(int on, uint16_t pa, uint16_t rxg)
{
    rf_regi2c_write_bits(0x67, 0x00, 6, 6, on ? 1 : 0);
    REG(PBUS_STAT) = (REG(PBUS_STAT) & ~(3u << 16)) | (on ? 3u << 16 : 0);
    REG(PBUS_STAT) = (REG(PBUS_STAT) & ~(3u << 14)) | (on ? 3u << 14 : 0);
    if (!on || pa == 0xFFFF) return;            /* 0xFFFF: switch and clocks only, keep the TX pbus rows */
    rf_pbus_write(4, 1, 127); rf_pbus_write(5, 1, pa); rf_pbus_write(0, 1, 0x104);
    rf_pbus_write(1, 1, 0x1F9); rf_pbus_write(1, 2, rxg);
}

/* baseband DC / power estimator (rom_iq_est_enable / rom_dc_iq_est / rom_iq_est_disable):
 * averages n+1 samples in hardware; works where the dump engine cannot (TX forced on) */
void rf_bb_est(uint16_t n, int32_t *i, int32_t *q, int32_t *pwr)
{
    REG(0x60006140) = (REG(0x60006140) & 0xF3FFFFFFu) | 1u << 26;
    REG(0x60006144) = (REG(0x60006144) & 0xFFE7FFFFu) | 1u << 20;
    REG(0x60006144) = (REG(0x60006144) & 0xFFFE0003u) | (uint32_t)(n & 0x7FFF) << 2;
    REG(0x60006144) |= 1;
    rf_delay_us(1);
    REG(0x60006144) |= 2;
    for (uint32_t t = 0; t < 1000000 && !(REG(0x60006174) & 1u << 16); t++) {}
    int32_t si = (int32_t)REG(0x6000615C) >> 6, sq = (int32_t)REG(0x60006160) >> 6;
    *i = si / (n + 1); *q = sq / (n + 1);
    *pwr = (int32_t)REG(0x60006164) / (n + 1) - (*i * *i + *q * *q);
    REG(0x60006144) &= ~2u;
    rf_delay_us(1);
    REG(0x60006144) &= ~1u;
}

/* Receive LO plan (after esp_sdr lo_plan.h): the VCO locks from ~2230 to
 * ~2760 MHz (measured, examples/rf_range). Below RF_LO_DIRECT_MIN_KHZ the PLL
 * runs at 6/5 of the wanted LO and analog 65.00[4] selects the 6/5 divider
 * path of the receive LO (transmit always uses the direct path). */
int rf_tune_rx_khz(uint32_t khz)
{
    int alt = khz < RF_LO_DIRECT_MIN_KHZ;
    rf_pll_result_t r;
    int rc = rf_pll_set_khz(alt ? khz / 5 * 6 + (khz % 5) * 6 / 5 : khz, &r);
    rf_regi2c_write_bits(0x65, 0x00, 4, 4, alt ? 1 : 0);
    return rc ? rc : (r.locked ? 0 : -3);
}

/* TX power detector through SAR2 (rom_en_pwdet + rom_pwdet_sar2_init);
 * rf_pwdet_read() sums the 8 results at 0x6000E080..9C (rom_read_sar_dout) */
void rf_pwdet_on(void)
{
    REG(0x6000E05C) &= ~(1u << 23);
    REG(0x6000E05C) &= ~(1u << 21);
    REG(0x6000E050) &= ~1u;
    REG(0x60006048) |= 0x80;
    REG(0x6000E050) |= 0xC0;
    REG(0x60008834) &= 0x7FFFFFFFu;
    REG(0x60008830) |= 0x80000000u;
    REG(0x60008830) &= 0x8007FFFFu;
    REG(0x6000E060) |= 0x18;
    REG(0x6000E060) &= ~0x20u;
    REG(0x6000E05C) = (REG(0x6000E05C) & 0xFFFF0000u) | 0x16A;
    REG(0x60008834) = (REG(0x60008834) & 0x8FFFFFFFu) | 0x40000000u;
}

uint32_t rf_pwdet_read(void)                   /* rom_pkdet_vol_start + rom_read_sar_dout */
{
    REG(0x6000E05C) |= 1u << 23;
    REG(0x6000E05C) |= 1u << 19;
    for (uint32_t t = 0; t < 100000 && (REG(0x6000E050) >> 24 & 7) != 7; t++) {}
    REG(0x6000E050) &= ~2u;
    REG(0x6000E050) |= 2u;
    rf_delay_us(10);
    for (uint32_t t = 0; t < 100000 && (REG(0x6000E050) >> 24 & 7) != 7; t++) {}
    REG(0x6000E05C) &= ~(1u << 23);
    REG(0x6000E05C) &= ~(1u << 19);
    uint32_t s = 0;
    for (unsigned k = 0; k < 8; k++) s += REG(0x6000E080u + 4 * k) & 0x1FFF;
    return s;
}

/* TX LO-leakage (DC) calibration with our own power detector: transmitting
 * with no DAC data, the detector output is the leakage; minimise it over the
 * two DC words of the TX row (w0 = lo[20:12], w2 = hi[31:26] + lo[2:0]),
 * coarse to fine, twice. Call after rf_tx_on(gain, bb). */
static uint32_t pwdet_avg(void) { uint32_t a = 0; for (int k = 0; k < 16; k++) a += rf_pwdet_read(); return a; }

static uint32_t try_dc(uint8_t gain, uint16_t bb, int w, int v)
{
    txcal[wifi_class(bb)][w] = (uint16_t)v;
    rf_tx_gain(gain, bb, 0);
    rf_txon(0); rf_txon(1);
    rf_delay_us(50);
    return pwdet_avg();
}

void rf_tx_dc_reset(void)                      /* neutral DC words (a chip without copied calibration) */
{
    rf_tx_gain(0, 0, 0);                        /* make sure the RAM table exists */
    for (int r = 0; r < 5; r++) { txcal[r][0] = 0x100; txcal[r][2] = 0x100; }
}

void rf_tx_dc_cal(uint8_t gain, uint16_t bb)
{
    rf_tx_gain(gain, bb, 0);                    /* initialises the RAM table */
    volatile uint32_t *z = (volatile uint32_t *)RF_DUMP_BANK(0);
    for (int k = 0; k < 64; k++) z[k] = 0;      /* a stopped DAC holds its last sample: feed it zeros */
    rf_dac_play(0, 64, 0, 1);
    rf_pwdet_on();
    unsigned c = wifi_class(bb);
    for (int pass = 0; pass < 2; pass++)
        for (int w = 0; w <= 2; w += 2) {
            int best = txcal[c][w];
            for (int step = pass ? 8 : 32; step >= 1; step /= 2)
                for (int moves = 0; moves < 16; moves++) {   /* left, centre, right measured together: the detector drifts */
                    int lo = best - step < 0 ? 0 : best - step, hi = best + step > 511 ? 511 : best + step;
                    uint32_t pl = try_dc(gain, bb, w, lo), pc = try_dc(gain, bb, w, best), ph = try_dc(gain, bb, w, hi);
                    if (pl < pc && pl <= ph) best = lo;
                    else if (ph < pc) best = hi;
                    else break;
                }
            try_dc(gain, bb, w, best);
        }
    rf_dac_stop();
}
