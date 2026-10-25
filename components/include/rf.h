/* RF: the ESP32-S3 radio from esp-ndf's own code (no vendor PHY/BT/Wi-Fi blobs).
 * One state machine (OFF -> IDLE -> RX or TX), units in kHz / dB, every call checked.
 * Transmit is fenced by limits (band, level); see rf_set_limits. Details and measurements:
 * docs/PHY_STANDALONE.md, docs/RF_API.md. Lower-level calls (components/drivers/rf/ headers) stay available
 * for bench work but bypass these checks. */
#pragma once
#include <stdint.h>

typedef enum {
    RF_OK = 0,
    RF_ERR_STATE = -1,      /* wrong state for this call (e.g. TX while closed) */
    RF_ERR_RANGE = -2,      /* argument outside what the hardware can do */
    RF_ERR_LOCK = -3,       /* synthesizer did not lock */
    RF_ERR_DENIED = -4,     /* outside the configured limits (band / level) */
    RF_ERR_TIMEOUT = -5,
    RF_ERR_BUSY = -6,
} rf_err_t;

typedef enum { RF_STATE_OFF, RF_STATE_IDLE, RF_STATE_RX, RF_STATE_TX } rf_state_t;

/* ---- lifecycle --------------------------------------------------------------------------------- */
int  rf_open(void);                      /* power, init, calibrations; -> IDLE */
void rf_close(void);                     /* everything off; -> OFF */
rf_state_t rf_state(void);

/* ---- calibration store -------------------------------------------------------------------------------
 * rf_open loads this chip's stored calibration (RX DC rows, TX DC table, crystal correction; one CRC-checked record in
 * the "phy" data partition) if the die temperature is within RF_CAL_TEMP_C of when it was made; otherwise it calibrates and
 * stores. rf_tx_start runs the TX DC calibration once and stores it. */
#define RF_CAL_TEMP_C 15.0f
enum { RF_CAL_LOADED = 1, RF_CAL_RUN = 2, RF_CAL_RUN_SAVED = 3 };
int  rf_cal_source(void);                /* how the last rf_open got its calibration */
int  rf_cal_check(void);                 /* temperature trigger (IDLE): recalibrate + store when moved > RF_CAL_TEMP_C; 1 if it did */
int  rf_cal_save(int tx_valid);
int  rf_cal_load(int *tx_valid);
int  rf_cal_erase(void);
float rf_cal_temp_c(void);
float rf_temp_c(void);

/* ---- limits (safety) ----------------------------------------------------------------------------
 * TX is refused outside [tx_min_khz, tx_max_khz] or above tx_max_db (dB relative to full scale).
 * Default: 2400000..2483500 kHz (2.4 GHz ISM), 0 dB. Widening is the caller's legal responsibility. */
/* tx_max_ms: longest continuous transmission (rf_tx_start until rf_tx_stop, or a continuous rf_pkt_send); when it
 * runs out the transmitter is switched off by a timer and the state returns to IDLE. 0 = no limit (default). */
typedef struct { uint32_t tx_min_khz, tx_max_khz; int tx_max_db; uint32_t tx_max_ms; } rf_limits_t;
int  rf_tx_expired(void);                /* 1 if the last transmission was ended by tx_max_ms */
void rf_set_limits(const rf_limits_t *l);
void rf_get_limits(rf_limits_t *l);

/* ---- synthesizer --------------------------------------------------------------------------------- */
/* Guaranteed range: both chips, 63..90 C (measured: direct VCO 2207.6/2213.9..2784.5/2788.3 MHz, the 6/5 divider
 * down to 1839.5/1844.8 MHz; edges move ~-2.5 MHz from 63 to 90 C). RX and TX both work across it (over the air). */
#define RF_FREQ_MIN_KHZ 1850000u
#define RF_FREQ_MAX_KHZ 2780000u
int  rf_set_freq_khz(uint32_t khz);      /* LO for RX and TX; calibrates the VCO (~0.4 ms) */
uint32_t rf_get_freq_khz(void);
int  rf_set_freq_hz(uint64_t hz);        /* Hz resolution: steps of 457.76 Hz (direct) / 381.47 Hz (divider), rounded */
uint64_t rf_get_freq_hz(void);
/* crystal correction: +ppb = the crystal runs fast; every tune is scaled by it (re-tunes now if open) */
void rf_set_xtal_ppb(int32_t ppb);
int32_t rf_get_xtal_ppb(void);
/* open-loop temperature compensation: ppb = c0 + c1*Tf + c2*Tf^2, Tf = die temperature through a first-order lag
 * tau_s; rf_xtal_track() applies it (call ~1 Hz). Measured limit ~+-0.4 ppm (docs/RF_API.md). */
void rf_set_xtal_temp_model(float c0, float c1, float c2, float tau_s);
int32_t rf_xtal_track(void);
/* closed loop: measure a known tone (Hz from the LO; RX state) and steer the crystal correction toward it */
int  rf_rx_tone_hz(float f_guess_hz, float *hz);
int32_t rf_afc_step(float offset_hz, float gain);     /* returns the new ppb; gain 0.5..1 */
typedef struct { uint32_t khz; uint32_t n; uint16_t cap; } rf_hop_t;
int  rf_hop_prepare(rf_hop_t *h, uint32_t khz);   /* calibrate once, keep the cap */
int  rf_pll_locked(void);                /* 1: VCO control voltage in window (no lock interrupt found: poll) */
int  rf_check_lock(void);                /* re-tunes once if unlocked; RF_OK or RF_ERR_LOCK */
uint32_t rf_relock_count(void);
int  rf_hop(const rf_hop_t *h);          /* retune with a prepared cap and divider: ~10 us (3 bus writes) within a band */

/* ---- receive --------------------------------------------------------------------------------------- */
#define RF_GAIN_AGC (-1)
int  rf_rx_start(void);                  /* IDLE -> RX */
int  rf_rx_set_gain(int index);          /* 0..127 table index, or RF_GAIN_AGC */
/* gain in dB above the minimum (0..RF_GAIN_DB_MAX, ~1 dB steps; the baseband AGC does not act on raw IQ) */
#define RF_GAIN_DB_MAX 45
int  rf_rx_set_gain_db(int db);
/* set the gain so the capture peak lands near target_peak (16..500 of 511); *db = the chosen gain. RX state. */
int  rf_rx_autogain(int target_peak, int *db);
int  rf_rx_set_bandwidth_khz(uint32_t khz);   /* analog: nearest corner >= khz: 7000..11000+ (no narrower setting exists); narrow channels: rf_rx_capture_dec */
enum { RF_RATE_80M = 0, RF_RATE_40M = 1, RF_RATE_16M = 2 };
/* one-shot capture of n IQ words (n <= RF_CAPTURE_MAX) at rate; *iq points at the samples (valid until the next capture) */
#define RF_CAPTURE_MAX 16383u
int  rf_rx_capture(int rate, uint32_t n, const uint32_t **iq);
/* gapless stream: cb gets two segments per unit (~12288 words); return nonzero to stop. Interrupts stay masked. */
typedef int (*rf_rx_cb)(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, void *ctx);
int  rf_rx_stream(int rate, uint32_t units, rf_rx_cb cb, void *ctx);
int  rf_rx_power(uint16_t n, int32_t *pwr);
/* long gapless capture into any buffer (internal RAM or PSRAM, see psram.h) via rf_rx_stream.
 * RF_FMT_IQ32: the raw 32-bit words (4 B/sample); RF_FMT_IQ8: I and Q as signed 8 bit (top 8 of 10 bits, 2 B/sample:
 * fits PSRAM at 16 MS/s). Returns RF_OK, or RF_ERR_TIMEOUT if the destination could not keep up (a gap). */
enum { RF_FMT_IQ32 = 0, RF_FMT_IQ8 = 1 };
int  rf_rx_capture_to(int rate, int fmt, uint32_t n_samples, void *dst);
/* triggered: capture into a circular buffer of n_samples until a sample with |I|+|Q| >= level arrives, then pre..n
 * samples later stop. *first = index (in samples) of the oldest sample in dst (the buffer is circular),
 * *trig = index of the trigger sample. Gives up after timeout_ms without a trigger (RF_ERR_TIMEOUT). */
/* decimated capture: the 16 MS/s stream through a channel filter (-6 dB at +/-0.375 x output rate, Hamming FIR of 8*decim
 * taps) and decimated by 8 (2 MS/s, ~1.2 MHz wide) or 16 (1 MS/s, ~0.6 MHz wide), centred on the LO. n output samples,
 * I and Q as int16 pairs (4 B/sample; PSRAM is fine). Gapless; RF_ERR_TIMEOUT if the filter fell behind. */
int  rf_rx_capture_dec(uint32_t decim, uint32_t n, int16_t *dst);
int  rf_rx_capture_trigger(int rate, int fmt, int level, uint32_t pre, uint32_t n_samples, void *dst,
                           uint32_t timeout_ms, uint32_t *first, uint32_t *trig);   /* hardware power estimate over n+1 samples (energy detect) */

/* ---- transmit ------------------------------------------------------------------------------------- */
int  rf_tx_start(void);                  /* IDLE -> TX (checks the band limit for the current LO) */
#define RF_TX_LEVEL_MIN_DB (-25)
int  rf_tx_set_level_db(int db);         /* -25..0 dB relative to full scale (digital gain; measured table) */
#define RF_TX_MAX_WORDS 16384u
/* play n IQ words at 40 MS/s from buf (copied into the DAC bank); loop repeats gaplessly until rf_tx_stop */
int  rf_tx_play(const uint32_t *buf, uint32_t n, int loop);
/* gapless stream at 40 MS/s: cb fills 8192 words (205 us of air time) per call, unit after unit; return nonzero to end.
 * The callback has ~75 us per unit (measured: 153 us window - 76 us tail copy; a table fill takes 45 us). Interrupts stay masked. RF_ERR_TIMEOUT = a gap. */
#define RF_TX_UNIT 8192u
typedef int (*rf_tx_cb)(uint32_t *buf, uint32_t n, void *ctx);
int  rf_tx_stream(uint32_t units, rf_tx_cb cb, void *ctx);
int  rf_tx_tone_khz(int offset_khz, int amplitude);   /* CW at LO + offset (-19000..19000 kHz), amplitude 1..511 */
int  rf_tx_stop(void);                   /* TX -> IDLE */

/* ---- packets (hardware, no CPU in the data path) -------------------------------------------------------
 * GFSK packets framed, CRC'd, whitened and stored by the radio's packet hardware (the ESP32-S3 link-layer core used
 * as a plain packet engine: no advertising, connections or any BLE protocol). On air: preamble, sync word (any 32 bits),
 * header byte, length byte, 0..255 payload bytes, 24-bit CRC (init crc_init; RF_PKT_CRC_OFF: none sent / none checked).
 * Whitening is on by default, seeded by the channel; RF_PKT_WHITEN_OFF sends / takes header, length and payload literally
 * (both ends must agree; long runs of equal bits then cost bit errors: the 0,1,2.. test payload loses ~74 % of packets).
 * Receive: one packet per hardware event, re-armed in ~0.5 ms: about 1000 short packets/s, every long packet.
 * Frequencies: 2402..2480 MHz in 2 MHz steps. Rates 1M, 2M, coded S8 (125 kbit/s), coded S2 (500 kbit/s). */
enum { RF_PKT_1M = 0, RF_PKT_2M = 1, RF_PKT_125K = 2, RF_PKT_500K = 3 };
#define RF_PKT_CRC_OFF    1u
#define RF_PKT_WHITEN_OFF 2u
/* power: TX power index 5..15 (0 = default 11). Measured output relative to index 15, dB (bench/rfmeas/tx_power_index.txt):
 * 5:-39 6:-37 7:-34 8:-25 9:-28 10:-25 11:-22 12:-17 13:-14 14:+1 15:0; indices 0..4 emit nothing usable. Sends above the
 * rf_set_limits level (tx_max_db, relative to full scale = index 15) are refused. */
typedef struct { uint16_t mhz; uint8_t rate; uint8_t flags; uint32_t sync; uint32_t crc_init; uint8_t power; } rf_pkt_cfg_t;
typedef struct { uint64_t time_us; uint8_t hdr, len; int8_t rssi; uint8_t ok; uint16_t status; uint8_t data[255]; } rf_pkt_t;   /* rssi: dBm (IDF scale); time_us: sync-word time on rf_time_us() */
/* send: count packets back to back (0 = until rf_pkt_stop); checked against the TX limits */
int  rf_pkt_send(const rf_pkt_cfg_t *c, uint8_t hdr, const uint8_t *data, unsigned len, uint32_t count);
/* receive: continuously; nothing is ever transmitted (the BT TX state's PA enable is cleared while receiving).
 * gain: RF_GAIN_AGC (-1) for hardware AGC, or 0..127 to hold the AGC at one gain */
int  rf_pkt_rx_start(const rf_pkt_cfg_t *c, int gain);
int  rf_pkt_read(rf_pkt_t *p);           /* 1 and the oldest packet (ok = CRC good), or 0 */
int  rf_pkt_busy(void);                  /* a counted send still running */
/* radio time base (the packet hardware's clock, 0.5 us resolution): available after the first rf_pkt_* call */
uint64_t rf_time_us(void);
/* send one packet starting at t_us on rf_time_us() (>= 300 us ahead) */
int  rf_pkt_send_at(const rf_pkt_cfg_t *c, uint8_t hdr, const uint8_t *data, unsigned len, uint64_t t_us);
/* 1 if sync follows the BLE access-address rules (no run of more than 6 equal bits, at most 24 transitions, at least 2
 * transitions in the top 6 bits, not 4 equal bytes): good correlation properties. Any 32-bit value is accepted. */
int  rf_pkt_sync_ok(uint32_t sync);
void rf_pkt_stop(void);

/* ---- raw 802.11 through the Wi-Fi MAC (channel 6), no vendor stack (4.2) ----------------------------
 * Our own code builds the frame, drives the MAC TX/RX descriptor path, and brings up the calibrated
 * carrier and front end. init once, then tx/rx. See docs/PENDING.md and bench/rfmeas/wifi_tx_42.txt. */
int  rf_wifi_tx_init(void);                      /* carrier + TX front end on, replay the TX config */
int  rf_wifi_tx(const uint8_t *frame, unsigned len);          /* send one 802.11 frame (10..1600 B) */
int  rf_wifi_rx_init(void);                      /* carrier + RX front end on, replay the RX config, arm our ring */
int  rf_wifi_rx(uint8_t *out, unsigned max, uint32_t timeout_ms);   /* next received frame -> out; returns length, 0 on timeout */

/* ---- 802.15.4 O-QPSK by DAC waveform (4.3) ---------------------------------------------------------
 * The S3 has no 802.15.4 radio; this synthesises the O-QPSK/DSSS baseband at 40 MS/s to play through
 * the TX DAC (rf_tx_play). Transmit only. Returns the IQ sample count, 0 if it won't fit in max. */
uint32_t rf_154_build(const uint8_t *psdu, unsigned len, uint32_t *out, uint32_t max);

/* ---- correlator trigger (4.4) ----------------------------------------------------------------------
 * Trigger on a known sync pattern (not just energy): slide-correlate a captured IQ window against a
 * reference complex waveform (refI/refQ, rlen taps). *peak_off = sync alignment (the trigger sample),
 * *peak_mag = peak power; returns the peak-to-mean power ratio x256 (detection confidence). */
int32_t rf_corr_trigger(const uint32_t *iq, uint32_t n, const int8_t *refI, const int8_t *refQ,
                        uint32_t rlen, uint32_t *peak_off, int64_t *peak_mag);

/* ---- samples ------------------------------------------------------------------------------------- */
/* IQ word: I bits 9:0, Q bits 19:10, signed 10-bit. rf_sample_q returns the true-spectrum Q (the raw Q bits are inverted). */
static inline int rf_sample_i(uint32_t w) { return (int32_t)(w << 22) >> 22; }
static inline int rf_sample_q(uint32_t w) { return -((int32_t)(w << 12) >> 22); }
static inline uint32_t rf_sample_word(int i, int q) { return ((uint32_t)i & 0x3FF) | ((uint32_t)q & 0x3FF) << 10; }
