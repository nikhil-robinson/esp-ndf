/* Public RF API (include/rf.h) over the drivers in this directory: state, limits, units. */
#include "rf.h"
#include "rf_init.h"
#include "rf_pll.h"
#include "rf_dump.h"
#include "rf_regi2c.h"
#include "sys.h"
#include "xtensa.h"
#include <string.h>
#include <math.h>

static rf_state_t state = RF_STATE_OFF;
static uint32_t freq_khz;
static int tx_db;
static rf_limits_t lim = { 2400000, 2483500, 0 };

rf_state_t rf_state(void) { return state; }
void rf_set_limits(const rf_limits_t *l) { lim = *l; }
void rf_get_limits(rf_limits_t *l) { *l = lim; }

int rf_open(void)
{
    if (state != RF_STATE_OFF) return RF_ERR_STATE;
    rf_init_replay();
    state = RF_STATE_IDLE;
    int rc = rf_set_freq_khz(2437000);
    if (rc) return rc;
    rf_rx_on();
    rf_rx_dc_cal(128);                           /* RX DC offsets for every gain row, at this LO */
    rf_rx_gain(RF_GAIN_AGC);
    return RF_OK;
}

void rf_close(void)
{
    if (state == RF_STATE_TX) rf_tx_stop();
    rf_dac_stop();
    rf_pbus_xpd_tx_off();
    state = RF_STATE_OFF;
}

int rf_set_freq_khz(uint32_t khz)
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    if (khz < RF_FREQ_MIN_KHZ || khz > RF_FREQ_MAX_KHZ) return RF_ERR_RANGE;
    if (state == RF_STATE_TX && (khz < lim.tx_min_khz || khz > lim.tx_max_khz)) return RF_ERR_DENIED;
    int rc = rf_tune_rx_khz(khz);
    if (rc == -1) return RF_ERR_RANGE;
    if (rc) return RF_ERR_LOCK;
    freq_khz = khz;
    return RF_OK;
}
uint32_t rf_get_freq_khz(void) { return freq_khz; }

int rf_hop_prepare(rf_hop_t *h, uint32_t khz)
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    rf_pll_result_t r;
    if (rf_pll_set_khz(khz, &r)) return RF_ERR_RANGE;
    if (!r.locked) return RF_ERR_LOCK;
    h->khz = khz; h->cap = r.cap;
    if (freq_khz) rf_pll_set_khz(freq_khz, &r);  /* back where we were */
    return RF_OK;
}

int rf_hop(const rf_hop_t *h)
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    if (state == RF_STATE_TX && (h->khz < lim.tx_min_khz || h->khz > lim.tx_max_khz)) return RF_ERR_DENIED;
    rf_pll_hop(h->khz, h->cap);
    freq_khz = h->khz;
    return RF_OK;
}

int rf_rx_start(void)
{
    if (state == RF_STATE_TX) rf_tx_stop();
    if (state != RF_STATE_IDLE && state != RF_STATE_RX) return RF_ERR_STATE;
    rf_rx_on();
    state = RF_STATE_RX;
    return RF_OK;
}

int rf_rx_set_gain(int index)
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    if (index > 127 || index < RF_GAIN_AGC) return RF_ERR_RANGE;
    rf_rx_gain(index);
    return RF_OK;
}

int rf_rx_set_bandwidth_khz(uint32_t khz)        /* 67.04 / 67.05 (I / Q): measured corners, docs/PHY_STANDALONE.md */
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    uint8_t code = khz <= 7000 ? 0x38 : khz <= 8000 ? 0x30 : khz <= 9000 ? 0x28 : khz <= 10000 ? 0x20 : 0x1F;
    rf_regi2c_write(0x67, 0x04, code);
    rf_regi2c_write(0x67, 0x05, code);
    return RF_OK;
}

static const uint32_t rates[] = { RF_DUMP_80M, RF_DUMP_40M, RF_DUMP_16M };

int rf_rx_capture(int rate, uint32_t n, const uint32_t **iq)
{
    if (state != RF_STATE_RX) return RF_ERR_STATE;
    if (rate < 0 || rate > 2 || !n || n > RF_CAPTURE_MAX) return RF_ERR_RANGE;
    if (rf_dump_burst(rates[rate], n)) return RF_ERR_TIMEOUT;
    *iq = (const uint32_t *)RF_DUMP_BURST_ADDR;
    return RF_OK;
}

int rf_rx_stream(int rate, uint32_t units, rf_rx_cb cb, void *ctx)
{
    if (state != RF_STATE_RX) return RF_ERR_STATE;
    if (rate < 0 || rate > 2 || !cb) return RF_ERR_RANGE;
    rf_stream_stats_t st;
    uint32_t ps = irq_lock();
    int rc = rf_stream(rates[rate], units, cb, ctx, &st);
    irq_unlock(ps);
    return rc ? RF_ERR_TIMEOUT : RF_OK;
}

int rf_rx_power(uint16_t n, int32_t *pwr)
{
    if (state != RF_STATE_RX) return RF_ERR_STATE;
    int32_t i, q;
    rf_bb_est(n, &i, &q, pwr);
    return RF_OK;
}

static uint8_t dig_code(int db)                  /* 0x80 = -25 dB .. 0xF0 = 0 dB (measured, ~linear) */
{
    return (uint8_t)(0x80 + (db + 25) * 0x70 / 25);
}

int rf_tx_start(void)
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    if (freq_khz < lim.tx_min_khz || freq_khz > lim.tx_max_khz) return RF_ERR_DENIED;
    if (tx_db > lim.tx_max_db) tx_db = lim.tx_max_db;
    rf_tx_on(127, 0);
    rf_tx_gain(127, 0, (int8_t)dig_code(tx_db));
    state = RF_STATE_TX;
    return RF_OK;
}

int rf_tx_set_level_db(int db)
{
    if (db < -25 || db > 0) return RF_ERR_RANGE;
    if (db > lim.tx_max_db) return RF_ERR_DENIED;
    tx_db = db;
    if (state == RF_STATE_TX) rf_tx_gain(127, 0, (int8_t)dig_code(db));
    return RF_OK;
}

int rf_tx_play(const uint32_t *buf, uint32_t n, int loop)
{
    if (state != RF_STATE_TX) return RF_ERR_STATE;
    if (!n || n > RF_TX_MAX_WORDS) return RF_ERR_RANGE;
    rf_dac_stop();
    memcpy((void *)RF_DUMP_BANK(1), buf, n * 4);
    rf_dac_play(1, n, 0, loop);
    return RF_OK;
}

int rf_tx_tone_khz(int offset_khz, int amplitude)
{
    if (state != RF_STATE_TX) return RF_ERR_STATE;
    if (offset_khz < -19000 || offset_khz > 19000 || amplitude < 1 || amplitude > 511) return RF_ERR_RANGE;
    /* whole cycles in the buffer: n samples at 40 MS/s hold an integer number of periods (offset multiple of 2.5 kHz) */
    uint32_t n = 16000;                          /* 400 us: offsets in steps of 2.5 kHz loop seamlessly */
    volatile uint32_t *b = (volatile uint32_t *)RF_DUMP_BANK(1);
    rf_dac_stop();
    for (uint32_t k = 0; k < n; k++) {
        float ph = 2 * (float)M_PI * (float)offset_khz * 1000.0f * (float)k / 40e6f;
        b[k] = rf_iq_word((int)lrintf(amplitude * cosf(ph)), (int)lrintf(amplitude * sinf(ph)));
    }
    rf_dac_play(1, n, 0, 1);
    return RF_OK;
}

int rf_tx_stop(void)
{
    if (state != RF_STATE_TX) return RF_ERR_STATE;
    rf_dac_stop();
    rf_tx_off();
    state = RF_STATE_IDLE;
    return RF_OK;
}
