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
int  rf_tx_tone_khz(int offset_khz, int amplitude);   /* CW at LO + offset (-19000..19000 kHz), amplitude 1..511 */
int  rf_tx_stop(void);                   /* TX -> IDLE */

/* ---- samples ------------------------------------------------------------------------------------- */
/* IQ word: I bits 9:0, Q bits 19:10, signed 10-bit. rf_sample_q returns the true-spectrum Q (the raw Q bits are inverted). */
static inline int rf_sample_i(uint32_t w) { return (int32_t)(w << 22) >> 22; }
static inline int rf_sample_q(uint32_t w) { return -((int32_t)(w << 12) >> 22); }
static inline uint32_t rf_sample_word(int i, int q) { return ((uint32_t)i & 0x3FF) | ((uint32_t)q & 0x3FF) << 10; }
