/* Public RF API (include/rf.h) over the drivers in this directory: state, limits, units. */
#include "rf.h"
#include "swtimer.h"
#include "ndf.h"
#include "rf_init.h"
#include "rf_pll.h"
#include "rf_dump.h"
#include "rf_regi2c.h"
#include "tsens.h"
#include "sys.h"
#include "xtensa.h"
#include "dsp.h"
#include <string.h>
#include <math.h>

static rf_state_t state = RF_STATE_OFF;
static uint32_t freq_khz;
static int tx_db, tx_dc_done, cal_src;
static rf_limits_t lim = { 2400000, 2483500, 0, 0 };
static swtimer_t tx_guard;
static volatile int tx_expired;
static void (*guard_stop)(void);
static void guard_fn(void *arg)                  /* timer ISR: transmitter off, nothing else touched */
{
    (void)arg;
    if (guard_stop) guard_stop();
    tx_expired = 1;
}
static void dac_guard_stop(void) { rf_dac_stop(); rf_txon(0); }
void rf_tx_guard(void (*stop)(void))             /* start the tx_max_ms guard for a transmission ended by stop() */
{
    tx_expired = 0;
    if (!lim.tx_max_ms) return;
    guard_stop = stop;
    swt_start(&tx_guard, lim.tx_max_ms * 1000u, false, guard_fn, 0);
}
void rf_tx_unguard(void) { swt_stop(&tx_guard); }
int rf_tx_expired(void) { return tx_expired; }
static int32_t xtal_ppb;                         /* crystal error: +ppb means the crystal runs fast */
static uint64_t freq_hz;
static uint32_t st_relocks;

rf_state_t rf_state(void)
{
    if (tx_expired && state == RF_STATE_TX) { rf_tx_off(); state = RF_STATE_IDLE; }   /* guard fired: finish the switch-off here */
    return state;
}
void rf_set_limits(const rf_limits_t *l) { lim = *l; }
void rf_get_limits(rf_limits_t *l) { *l = lim; }

int rf_open(void)
{
    if (state != RF_STATE_OFF) return RF_ERR_STATE;
    rf_init_replay();
    state = RF_STATE_IDLE;
    int rc = rf_set_freq_khz(2437000);    /* LO parked mid-band */
    if (rc) return rc;
    rf_rx_on();
    int txv = 0;
    cal_src = RF_CAL_LOADED;
    if (!rf_cal_load(&txv)) {                    /* stored for this chip and temperature? else calibrate and store */
        rf_rx_dc_cal(128);                       /* RX DC offsets for every gain row, at this LO */
        cal_src = rf_cal_save(0) ? RF_CAL_RUN : RF_CAL_RUN_SAVED;
    } else if (txv) tx_dc_done = 1;
    rf_rx_gain(RF_GAIN_AGC);
    return RF_OK;
}

int rf_cal_source(void) { return cal_src; }

/* temperature trigger: recalibrate (RX DC now, TX DC at the next rf_tx_start) and store when the die moved more than
 * RF_CAL_TEMP_C since the stored calibration. Call from time to time in IDLE. Returns 1 if it recalibrated. */
int rf_cal_check(void)
{
    if (state != RF_STATE_IDLE) return 0;
    float d = rf_temp_c() - rf_cal_temp_c();
    if (d < RF_CAL_TEMP_C && d > -RF_CAL_TEMP_C) return 0;
    uint32_t khz = freq_khz;
    rf_set_freq_khz(2437000); rf_rx_on(); rf_rx_dc_cal(128); rf_rx_gain(RF_GAIN_AGC);
    tx_dc_done = 0; rf_cal_save(0);
    rf_set_freq_khz(khz);
    return 1;
}

void rf_close(void)
{
    if (state == RF_STATE_TX) rf_tx_stop();
    rf_dac_stop();
    rf_pbus_xpd_tx_off();
    tx_dc_done = 0;
    state = RF_STATE_OFF;
}

int rf_set_freq_khz(uint32_t khz)
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    if (khz < RF_FREQ_MIN_KHZ || khz > RF_FREQ_MAX_KHZ) return RF_ERR_RANGE;
    if (state == RF_STATE_TX && (khz < lim.tx_min_khz || khz > lim.tx_max_khz)) return RF_ERR_DENIED;
    return rf_set_freq_hz((uint64_t)khz * 1000u);
}

int rf_set_freq_hz(uint64_t hz)
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    if (hz < (uint64_t)RF_FREQ_MIN_KHZ * 1000u || hz > (uint64_t)RF_FREQ_MAX_KHZ * 1000u) return RF_ERR_RANGE;
    if (state == RF_STATE_TX && (hz < (uint64_t)lim.tx_min_khz * 1000u || hz > (uint64_t)lim.tx_max_khz * 1000u)) return RF_ERR_DENIED;
    int64_t corr = (int64_t)hz * xtal_ppb / 1000000000;          /* program what the real crystal turns into hz */
    int rc = rf_tune_hz((uint64_t)((int64_t)hz - corr));
    if (rc == -3) { rc = rf_tune_hz((uint64_t)((int64_t)hz - corr)); }   /* one retry: a rare failed calibration (measured once in ~10^3) */
    if (rc == -1) return RF_ERR_RANGE;
    if (rc) return RF_ERR_LOCK;
    freq_hz = hz; freq_khz = (uint32_t)(hz / 1000u);
    return RF_OK;
}
uint64_t rf_get_freq_hz(void) { return freq_hz; }
/* small corrections (< 20 ppm) retune the divider only, keeping the VCO cap: no recalibration, no glitch beyond
 * the bus writes. The 6/5 divider path is scaled the same way. */
void rf_set_xtal_ppb(int32_t ppb)
{
    xtal_ppb = ppb;
    if (state == RF_STATE_OFF || !freq_hz) return;
    int64_t hz = (int64_t)freq_hz - (int64_t)freq_hz * xtal_ppb / 1000000000;
    if (freq_hz < (uint64_t)RF_LO_DIRECT_MIN_KHZ * 1000u) hz = hz * 6 / 5;
    rf_pll_hop_n(rf_pll_n_of_hz((uint64_t)hz), rf_pll_cap());
}

/* open-loop temperature model: the die sensor leads the crystal (first-order lag tau) and crystals are curved,
 * so ppb = c0 + c1*Tf + c2*Tf^2 on the lagged temperature Tf. Measured limit on D0: ~+-0.4 ppm (transmit-on
 * transients pull the crystal in ways the die sensor cannot see); use rf_afc_step against a known signal for better. */
static float tm_c0, tm_c1, tm_c2, tm_tau, tm_tf; static uint64_t tm_last_us; static int tm_on;
void rf_set_xtal_temp_model(float c0, float c1, float c2, float tau_s) { tm_c0 = c0; tm_c1 = c1; tm_c2 = c2; tm_tau = tau_s; tm_on = 1; tm_last_us = 0; }
int32_t rf_xtal_track(void)                      /* call ~1 Hz */
{
    if (!tm_on) return xtal_ppb;
    float t = tsens_read_c(); uint64_t now = time_us();
    if (!tm_last_us || tm_tau <= 0) tm_tf = t;
    else tm_tf += (1.0f - expf(-(float)(now - tm_last_us) * 1e-6f / tm_tau)) * (t - tm_tf);
    tm_last_us = now;
    int32_t ppb = (int32_t)lrintf(tm_c0 + tm_c1 * tm_tf + tm_c2 * tm_tf * tm_tf);
    if (ppb - xtal_ppb > 5 || xtal_ppb - ppb > 5) rf_set_xtal_ppb(ppb);
    return xtal_ppb;
}

/* frequency of a tone near f_guess (Hz from the LO): phase slope over 16 blocks of 1023 samples at 80 MS/s */
int rf_rx_tone_hz(float f_guess_hz, float *hz)
{
    const uint32_t *w; int rc = rf_rx_capture(RF_RATE_80M, 16 * 1023, &w);
    if (rc) return rc;
    double ph[16], c = cos(-2 * M_PI * f_guess_hz / 80e6), sn = sin(-2 * M_PI * f_guess_hz / 80e6), cr = 1, ci = 0;
    for (int b = 0; b < 16; b++) {                 /* one rotator across all blocks: a per-block restart adds a false phase step */
        double re = 0, im = 0;
        for (int k = 0; k < 1023; k++) {
            uint32_t x = w[b * 1023 + k]; double i = rf_sample_i(x), q = rf_sample_q(x);
            double rot_r = i * cr - q * ci, rot_i = i * ci + q * cr;   /* x * e^{-j 2 pi f0 t} */
            re += rot_r; im += rot_i;
            double t = cr * c - ci * sn; ci = ci * c + cr * sn; cr = t;
        }
        double m = 1.0 / sqrt(cr * cr + ci * ci); cr *= m; ci *= m;
        ph[b] = atan2(im, re);
    }
    double s = 0;
    for (int b = 1; b < 16; b++) { double d = ph[b] - ph[b - 1]; while (d > M_PI) d -= 2 * M_PI; while (d < -M_PI) d += 2 * M_PI; s += d; }
    *hz = f_guess_hz + (float)(s / 15 / (2 * M_PI) * 80e6 / 1023);
    return RF_OK;
}

/* automatic frequency control: a known signal measured offset_hz high means our LO is low -> crystal slow */
int32_t rf_afc_step(float offset_hz, float gain)
{
    if (!freq_hz) return xtal_ppb;
    int32_t d = (int32_t)lrintf(-offset_hz * gain * 1e9f / (float)freq_hz);
    rf_set_xtal_ppb(xtal_ppb + d);
    return xtal_ppb;
}
int32_t rf_get_xtal_ppb(void) { return xtal_ppb; }
uint32_t rf_get_freq_khz(void) { return freq_khz; }

int rf_hop_prepare(rf_hop_t *h, uint32_t khz)
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    rf_pll_result_t r;
    if (rf_pll_set_khz(khz, &r)) return RF_ERR_RANGE;
    if (!r.locked) return RF_ERR_LOCK;
    h->khz = khz; h->cap = r.cap; h->n = rf_pll_n_of_hz((uint64_t)khz * 1000u);
    if (freq_khz) rf_pll_set_khz(freq_khz, &r);  /* back where we were */
    return RF_OK;
}

RAM_FN int rf_hop(const rf_hop_t *h)
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    if (state == RF_STATE_TX && (h->khz < lim.tx_min_khz || h->khz > lim.tx_max_khz)) return RF_ERR_DENIED;
    rf_pll_hop_n(h->n, h->cap);
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

/* Gain-memory index for each dB of RX gain above index 0 (measured CW sweep, bench/rfmeas/rx_gain_table.csv): steps are
 * 1 dB +-0.2 except 33 -> 36.6 -> 39.6 (front-end switch to code 0xA4). */
static const uint8_t gain_idx_of_db[RF_GAIN_DB_MAX + 1] = { 0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 30, 31, 32,
    35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 45, 46, 46, 46, 46, 47, 47, 48, 49, 50, 51, 52 };
static int gain_db_now;
int rf_rx_set_gain_db(int db)
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    if (db < 0 || db > RF_GAIN_DB_MAX) return RF_ERR_RANGE;
    gain_db_now = db;
    rf_rx_gain(gain_idx_of_db[db]);
    return RF_OK;
}

/* Raw-IQ gain control (the baseband AGC only acts on Wi-Fi packets): capture, compare the peak with target, step the
 * gain by the difference in dB; up to 6 rounds. */
int rf_rx_autogain(int target_peak, int *db)
{
    if (state != RF_STATE_RX) return RF_ERR_STATE;
    if (target_peak < 16 || target_peak > 500) return RF_ERR_RANGE;
    int g = gain_db_now;
    for (int round = 0; round < 6; round++) {
        rf_rx_gain(gain_idx_of_db[g]); delay_us(50);
        const uint32_t *iq;
        if (rf_rx_capture(RF_RATE_40M, 2048, &iq)) return RF_ERR_TIMEOUT;
        int pk = 1;
        for (uint32_t k = 0; k < 2048; k++) { int i = rf_sample_i(iq[k]), q = rf_sample_q(iq[k]); if (i < 0) i = -i; if (q < 0) q = -q; if (i > pk) pk = i; if (q > pk) pk = q; }
        int step = pk >= 500 ? -10 : (int)lrintf(20 * log10f((float)target_peak / (float)pk));
        if (step > -2 && step < 2) break;
        int ng = g + step; if (ng < 0) ng = 0; if (ng > RF_GAIN_DB_MAX) ng = RF_GAIN_DB_MAX;
        if (ng == g) break;
        g = ng;
    }
    gain_db_now = g; rf_rx_gain(gain_idx_of_db[g]);
    if (db) *db = g;
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

int rf_cap_last_rc; uint32_t rf_cap_last_late, rf_cap_last_units;   /* bench: last capture's rf_stream result */
typedef struct {
    uint8_t *dst; uint32_t n, pos; int fmt;       /* pos: samples written (circular when trig mode) */
    int level; uint32_t pre, post_left, trig_at; int triggered, armed_trig; uint32_t deadline_units, units;
} cap_t;
static inline void put(cap_t *c, uint32_t i, uint32_t w)
{
    if (c->fmt == RF_FMT_IQ32) ((uint32_t *)c->dst)[i] = w;
    else ((uint16_t *)c->dst)[i] = (uint16_t)(((w >> 2) & 0xFF) | ((w >> 12) & 0xFF) << 8);   /* I[9:2], Q[19:12] */
}
static int cap_fast(cap_t *c, const uint32_t *a, uint32_t na)   /* plain capture: no per-sample branch */
{
    uint32_t left = c->n - c->pos, m = na < left ? na : left;
    if (c->fmt == RF_FMT_IQ32) memcpy((uint32_t *)c->dst + c->pos, a, m * 4);
    else {
        uint32_t *o = (uint32_t *)((uint16_t *)c->dst + c->pos), k = 0;
        if (c->pos & 1) { put(c, c->pos, a[0]); o = (uint32_t *)((uint16_t *)c->dst + c->pos + 1); k = 1; }
        for (; k + 1 < m; k += 2) {
            uint32_t w0 = a[k], w1 = a[k + 1];
            *o++ = ((w0 >> 2) & 0xFF) | ((w0 >> 4) & 0xFF00) | ((w1 << 14) & 0xFF0000) | ((w1 << 12) & 0xFF000000);   /* I8 Q8 | I8 Q8 */
        }
        if (k < m) put(c, c->pos + k, a[k]);
    }
    c->pos += m;
    return c->pos >= c->n;
}
/* triggered: circular writes in chunks through the fast path; the level is checked on every 16th sample
 * (1 us at 16 MS/s; a burst shorter than 16 samples can be missed) to stay inside the stream's time budget */
static int cap_trig(cap_t *c, const uint32_t *a, uint32_t na)
{
    uint32_t k = 0;
    if (!c->triggered) {
        for (uint32_t j = 0; j < na; j += 16) {
            uint32_t w = a[j]; int i = rf_sample_i(w), q = rf_sample_i(w >> 10);
            if ((i < 0 ? -i : i) + (q < 0 ? -q : q) >= c->level) { c->triggered = 1; c->trig_at = (c->pos + j) % c->n; c->post_left = c->n - c->pre + j; break; }
        }
    }
    while (k < na) {                                   /* write [k, na) into the ring, split at the wrap */
        uint32_t at = c->pos % c->n, room = c->n - at, m = na - k < room ? na - k : room;
        cap_t t = *c; t.pos = at; t.n = at + m;        /* a linear window of the ring */
        cap_fast(&t, a + k, m);
        c->pos += m; k += m;
    }
    if (c->triggered) { if (c->post_left <= na) return 1; c->post_left -= na; }
    return 0;
}
static int cap_seg(cap_t *c, const uint32_t *a, uint32_t na)
{
    if (!c->armed_trig) return cap_fast(c, a, na);
    return cap_trig(c, a, na);
    for (uint32_t k = 0; k < na; k++) {
        uint32_t w = a[k];
        if (c->armed_trig && !c->triggered) {
            int i = rf_sample_i(w), q = rf_sample_i(w >> 10);
            if ((i < 0 ? -i : i) + (q < 0 ? -q : q) >= c->level) { c->triggered = 1; c->trig_at = c->pos % c->n; c->post_left = c->n - c->pre; }
        }
        put(c, c->armed_trig ? c->pos % c->n : c->pos, w);
        c->pos++;
        if (c->armed_trig) { if (c->triggered && !--c->post_left) return 1; }
        else if (c->pos >= c->n) return 1;
    }
    return 0;
}
static int cap_cb(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, void *ctx)
{
    cap_t *c = ctx;
    if (cap_seg(c, a, na) || cap_seg(c, b, nb)) return 1;
    return c->armed_trig && !c->triggered && ++c->units >= c->deadline_units ? 2 : 0;
}
static const uint32_t rate_sps[3] = { 80000000u, 40000000u, 16000000u };
int rf_rx_capture_to(int rate, int fmt, uint32_t n, void *dst)
{
    if (state != RF_STATE_RX) return RF_ERR_STATE;
    if (rate < 0 || rate > 2 || fmt < 0 || fmt > 1 || !n || !dst) return RF_ERR_RANGE;
    cap_t c = { .dst = dst, .n = n, .fmt = fmt };
    rf_stream_stats_t st;
    uint32_t ps = irq_lock();
    int rc = rf_stream(rates[rate], 0xFFFFFFFFu, cap_cb, &c, &st);   /* until the callback stops it */
    irq_unlock(ps);
    rf_cap_last_rc = rc; rf_cap_last_late = st.late_max; rf_cap_last_units = st.units;
    if ((uint32_t)dst >= 0x3D000000u && (uint32_t)dst < 0x3E000000u) { extern void psram_writeback(void *p, size_t n); psram_writeback(dst, n * (fmt ? 2 : 4)); }
    return rc ? RF_ERR_TIMEOUT : RF_OK;
}
int rf_rx_capture_trigger(int rate, int fmt, int level, uint32_t pre, uint32_t n, void *dst, uint32_t timeout_ms, uint32_t *first, uint32_t *trig)
{
    if (state != RF_STATE_RX) return RF_ERR_STATE;
    if (rate < 0 || rate > 2 || fmt < 0 || fmt > 1 || !n || !dst || pre >= n || level < 1) return RF_ERR_RANGE;
    cap_t c = { .dst = dst, .n = n, .fmt = fmt, .level = level, .pre = pre, .armed_trig = 1 };
    c.deadline_units = (uint32_t)((uint64_t)timeout_ms * rate_sps[rate] / 1000 / 12288) + 1;
    rf_stream_stats_t st;
    uint32_t ps = irq_lock();
    int rc = rf_stream(rates[rate], 0xFFFFFFFFu, cap_cb, &c, &st);   /* until the callback stops it */
    irq_unlock(ps);
    if ((uint32_t)dst >= 0x3D000000u && (uint32_t)dst < 0x3E000000u) { extern void psram_writeback(void *p, size_t n); psram_writeback(dst, n * (fmt ? 2 : 4)); }
    if (!c.triggered) return RF_ERR_TIMEOUT;
    if (first) *first = c.pos >= n ? c.pos % n : 0;
    if (trig) *trig = c.trig_at;
    return rc ? RF_ERR_TIMEOUT : RF_OK;
}

/* Decimating channel filter on the 16 MS/s stream (rf.h rf_rx_capture_dec): Hamming-window FIR (48 taps at D 8, 128 at
 * D 16: what fits the ~15 cycles per input sample at 16 MS/s), computed only at the output points. D = 8 or 16 keeps every window 16-byte aligned. */
#define DEC_CH 1024u                                   /* input samples per filter pass */
typedef struct {
    int16_t bi[128 + DEC_CH] __attribute__((aligned(16))), bq[128 + DEC_CH] __attribute__((aligned(16)));
    int16_t h[128] __attribute__((aligned(16)));
    uint32_t T, D, fill, n, pos;
    int16_t *dst;
} dec_t;
static dec_t dec;
static void dec_taps(dec_t *d)
{
    float fc = 0.75f / (float)d->D / 2, sum = 0, t[128];   /* cutoff (-6 dB) at 0.75 x the output Nyquist */
    for (uint32_t k = 0; k < d->T; k++) {
        float x = (float)k - (float)(d->T - 1) / 2;
        float s = x == 0 ? 2 * fc : sinf(2 * 3.14159265f * fc * x) / (3.14159265f * x);
        t[k] = s * (0.54f - 0.46f * cosf(2 * 3.14159265f * (float)k / (float)(d->T - 1))); sum += t[k];
    }
    for (uint32_t k = 0; k < d->T; k++) d->h[k] = (int16_t)lrintf(t[k] / sum * 32767);   /* unity DC gain, Q15 */
}
static int dec_seg(dec_t *d, const uint32_t *a, uint32_t na)
{
    for (uint32_t k = 0; k < na; ) {
        uint32_t m = DEC_CH - d->fill < na - k ? DEC_CH - d->fill : na - k;
        int16_t *pi = d->bi + d->T + d->fill, *pq = d->bq + d->T + d->fill;
        if (!(((uint32_t)(a + k) | (uint32_t)pi | (uint32_t)pq) & 15) && !(m & 7)) dsp_unpack_iq10(a + k, pi, pq, (int)m);
        else for (uint32_t j = 0; j < m; j++) { uint32_t w = a[k + j]; pi[j] = (int16_t)((int32_t)(w << 22) >> 22); pq[j] = (int16_t)((int32_t)(w << 12) >> 22); }
        k += m; d->fill += m;
        if (d->fill < DEC_CH) break;
        for (uint32_t e = d->D; e <= DEC_CH; e += d->D) {             /* window [e, e + T) ends at a decimation point */
            int32_t r[2]; dsp_dot2_s16(d->bi + e, d->bq + e, d->h, (int)d->T, r);
            d->dst[2 * d->pos] = (int16_t)(r[0] >> 15); d->dst[2 * d->pos + 1] = (int16_t)(-(r[1] >> 15));   /* raw Q is inverted */
            if (++d->pos >= d->n) return 1;
        }
        memcpy(d->bi, d->bi + DEC_CH, d->T * 2); memcpy(d->bq, d->bq + DEC_CH, d->T * 2);
        d->fill = 0;
    }
    return 0;
}
static int dec_cb(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, void *ctx)
{
    return dec_seg(ctx, a, na) || dec_seg(ctx, b, nb);
}
uint32_t rf_dec_bench(uint32_t decim, const uint32_t *in, uint32_t n_in, int16_t *dst, uint32_t n_out)   /* bench: cycles for n_in samples */
{
    dec_t *d = &dec;
    if (d->D != decim) { d->D = decim; d->T = decim == 8 ? 48 : 128; dec_taps(d); }
    d->fill = 0; d->pos = 0; d->n = n_out; d->dst = dst;
    uint32_t t0 = ccount(); dec_seg(d, in, n_in); return ccount() - t0;
}
int rf_rx_capture_dec(uint32_t decim, uint32_t n, int16_t *dst)
{
    if (state != RF_STATE_RX) return RF_ERR_STATE;
    if ((decim != 8 && decim != 16) || !n || !dst) return RF_ERR_RANGE;
    dec_t *d = &dec;
    if (d->D != decim) { d->D = decim; d->T = decim == 8 ? 48 : 128; dec_taps(d); }
    memset(d->bi, 0, sizeof d->bi); memset(d->bq, 0, sizeof d->bq);
    d->fill = 0; d->pos = 0; d->n = n; d->dst = dst;
    rf_stream_stats_t st;
    uint32_t ps = irq_lock();
    int rc = rf_stream(RF_DUMP_16M, 0xFFFFFFFFu, dec_cb, d, &st);
    irq_unlock(ps);
    rf_cap_last_rc = rc; rf_cap_last_late = st.late_max; rf_cap_last_units = st.units;
    if ((uint32_t)dst >= 0x3D000000u && (uint32_t)dst < 0x3E000000u) { extern void psram_writeback(void *p, size_t n); psram_writeback(dst, n * 4); }
    return rc ? RF_ERR_TIMEOUT : RF_OK;
}

int rf_rx_power(uint16_t n, int32_t *pwr)
{
    if (state != RF_STATE_RX) return RF_ERR_STATE;
    int32_t i, q;
    rf_bb_est(n, &i, &q, pwr);
    return RF_OK;
}

/* TX digital gain byte per dB below full level: measured against code 0xF8 (the maximum) taken right before each code,
 * so the chip's slow output drift (+1 dB over 2 min) cancels (bench/rfmeas/tx_dig_sweep2.txt); table error <= 0.21 dB. */
static const uint8_t level_code[26] = { 0xF8, 0xF0, 0xEA, 0xE4, 0xDA, 0xD4, 0xD0, 0xCC, 0xC8, 0xC4, 0xC0, 0xBC, 0xB8, 0xB4, 0xB0, 0xAC,
                                        0xA8, 0xA4, 0xA0, 0x9C, 0x98, 0x94, 0x90, 0x8C, 0x88, 0x84 };
static uint8_t dig_code(int db) { return level_code[-db]; }

int rf_tx_start(void)
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    if (freq_khz < lim.tx_min_khz || freq_khz > lim.tx_max_khz) return RF_ERR_DENIED;
    if (tx_db > lim.tx_max_db) tx_db = lim.tx_max_db;
    rf_tx_on(127, 0);
    if (!tx_dc_done) { rf_tx_dc_cal(127, 0); tx_dc_done = 1; rf_cal_save(1); }   /* LO leakage -18.7 -> -28.4 dBc, holds 2402..2480 (measured); ~56 ms, then stored */
    rf_tx_gain(127, 0, (int8_t)dig_code(tx_db));
    state = RF_STATE_TX;
    rf_tx_guard(dac_guard_stop);
    return RF_OK;
}

int rf_tx_set_level_db(int db)
{
    if (db < RF_TX_LEVEL_MIN_DB || db > 0) return RF_ERR_RANGE;
    if (db > lim.tx_max_db) return RF_ERR_DENIED;
    tx_db = db;
    if (state == RF_STATE_TX) rf_tx_gain(127, 0, (int8_t)dig_code(db));
    return RF_OK;
}

int rf_tx_play(const uint32_t *buf, uint32_t n, int loop)
{
    if (rf_state() != RF_STATE_TX) return RF_ERR_STATE;
    if (!n || n > RF_TX_MAX_WORDS) return RF_ERR_RANGE;
    rf_dac_stop();
    memcpy((void *)RF_DUMP_BANK(1), buf, n * 4);
    rf_dac_play(1, n, 0, loop);
    return RF_OK;
}

int rf_tx_stream(uint32_t units, rf_tx_cb cb, void *ctx)
{
    if (state != RF_STATE_TX) return RF_ERR_STATE;
    if (!cb || units < 2) return RF_ERR_RANGE;
    rf_dac_stop();
    rf_dac_stats_t st;
    uint32_t ps = irq_lock();
    int rc = rf_dac_stream(units, cb, ctx, &st);
    irq_unlock(ps);
    return rc ? RF_ERR_TIMEOUT : RF_OK;
}

int rf_tx_tone_khz(int offset_khz, int amplitude)
{
    if (rf_state() != RF_STATE_TX) return RF_ERR_STATE;
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
    rf_tx_unguard();
    rf_dac_stop();
    rf_tx_off();
    state = RF_STATE_IDLE;
    return RF_OK;
}

/* lock: the VCO control voltage in the comparator window (62.0C[3:2] == 0, the rf_pll rule) */
int rf_pll_locked(void) { return (rf_regi2c_read(0x62, 0x0C) & 0x0C) == 0; }
int rf_check_lock(void)                          /* re-tune once if the loop slipped; RF_ERR_LOCK if that fails too */
{
    if (state == RF_STATE_OFF) return RF_ERR_STATE;
    if (rf_pll_locked()) return RF_OK;
    st_relocks++;
    return freq_hz ? rf_set_freq_hz(freq_hz) : RF_ERR_LOCK;
}
uint32_t rf_relock_count(void) { return st_relocks; }
