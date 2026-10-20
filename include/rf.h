/* RF: the ESP32-S3 radio from esp-ndf's own code (no vendor PHY/BT/Wi-Fi blobs).
 * One state machine (OFF -> IDLE -> RX or TX), units in kHz / dB, every call checked.
 * Transmit is fenced by limits (band, level); see rf_set_limits. Details and measurements:
 * docs/PHY_STANDALONE.md, docs/RF_API.md. Lower-level calls (drivers/rf/ headers) stay available
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

/* ---- limits (safety) ----------------------------------------------------------------------------
 * TX is refused outside [tx_min_khz, tx_max_khz] or above tx_max_db (dB relative to full scale).
 * Default: 2400000..2483500 kHz (2.4 GHz ISM), 0 dB. Widening is the caller's legal responsibility. */
typedef struct { uint32_t tx_min_khz, tx_max_khz; int tx_max_db; } rf_limits_t;
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
int  rf_rx_set_bandwidth_khz(uint32_t khz);   /* nearest corner >= khz: 7000..11000+ (measured codes) */
enum { RF_RATE_80M = 0, RF_RATE_40M = 1, RF_RATE_16M = 2 };
/* one-shot capture of n IQ words (n <= RF_CAPTURE_MAX) at rate; *iq points at the samples (valid until the next capture) */
#define RF_CAPTURE_MAX 16383u
int  rf_rx_capture(int rate, uint32_t n, const uint32_t **iq);
/* gapless stream: cb gets two segments per unit (~12288 words); return nonzero to stop. Interrupts stay masked. */
typedef int (*rf_rx_cb)(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, void *ctx);
int  rf_rx_stream(int rate, uint32_t units, rf_rx_cb cb, void *ctx);
int  rf_rx_power(uint16_t n, int32_t *pwr);   /* hardware power estimate over n+1 samples (energy detect) */

/* ---- transmit ------------------------------------------------------------------------------------- */
int  rf_tx_start(void);                  /* IDLE -> TX (checks the band limit for the current LO) */
int  rf_tx_set_level_db(int db);         /* -25..0 dB relative to full scale (digital gain, ~0.2 dB steps) */
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
 * Whitening is always on, seeded by the channel (RF_PKT_WHITEN_OFF is refused: no clean switch found yet).
 * Receive: one packet per hardware event, re-armed in ~0.5 ms: about 1000 short packets/s, every long packet.
 * Frequencies: 2402..2480 MHz in 2 MHz steps. Rates 1M, 2M, coded S8 (125 kbit/s), coded S2 (500 kbit/s). */
enum { RF_PKT_1M = 0, RF_PKT_2M = 1, RF_PKT_125K = 2, RF_PKT_500K = 3 };
#define RF_PKT_CRC_OFF    1u
#define RF_PKT_WHITEN_OFF 2u
typedef struct { uint16_t mhz; uint8_t rate; uint8_t flags; uint32_t sync; uint32_t crc_init; } rf_pkt_cfg_t;
typedef struct { uint8_t hdr, len; int8_t rssi; uint8_t ok; uint16_t status; uint8_t data[255]; } rf_pkt_t;
/* send: count packets back to back (0 = until rf_pkt_stop); checked against the TX limits */
int  rf_pkt_send(const rf_pkt_cfg_t *c, uint8_t hdr, const uint8_t *data, unsigned len, uint32_t count);
/* receive: continuously; the front end is held in receive (nothing is ever transmitted). gain: forced RX gain code
 * (127 measured good at bench distance; fixed gain, no AGC yet: RX chain work, docs/RF_API.md) */
int  rf_pkt_rx_start(const rf_pkt_cfg_t *c, int gain);
int  rf_pkt_read(rf_pkt_t *p);           /* 1 and the oldest packet (ok = CRC good), or 0 */
int  rf_pkt_busy(void);                  /* a counted send still running */
/* 1 if sync follows the BLE access-address rules (no run of more than 6 equal bits, at most 24 transitions, at least 2
 * transitions in the top 6 bits, not 4 equal bytes): good correlation properties. Any 32-bit value is accepted. */
int  rf_pkt_sync_ok(uint32_t sync);
void rf_pkt_stop(void);

/* ---- samples ------------------------------------------------------------------------------------- */
/* IQ word: I bits 9:0, Q bits 19:10, signed 10-bit. rf_sample_q returns the true-spectrum Q (the raw Q bits are inverted). */
static inline int rf_sample_i(uint32_t w) { return (int32_t)(w << 22) >> 22; }
static inline int rf_sample_q(uint32_t w) { return -((int32_t)(w << 12) >> 22); }
static inline uint32_t rf_sample_word(int i, int q) { return ((uint32_t)i & 0x3FF) | ((uint32_t)q & 0x3FF) << 10; }
