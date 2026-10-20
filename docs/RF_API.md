# RF API (`include/rf.h`)

The public interface to the ESP32-S3 radio in esp-ndf. No vendor blobs. Every call checks the state and its arguments and returns an `rf_err_t`. The bench-level drivers in `drivers/rf/` remain available, but they bypass these checks.

## States
`rf_open()` → IDLE → `rf_rx_start()` → RX, or `rf_tx_start()` → TX; `rf_tx_stop()` → IDLE; `rf_close()` → OFF.

## Calls

| Area | Calls | Notes (measured) |
|---|---|---|
| Lifecycle | `rf_open`, `rf_close`, `rf_state` | `rf_open`: init replay, LO at 2437 MHz, RX DC calibration for all 128 gain rows |
| Limits | `rf_set_limits`, `rf_get_limits` | TX is denied outside 2400.0–2483.5 MHz or above 0 dB unless the limits are widened by the caller |
| Synthesizer | `rf_set_freq_khz` / `rf_set_freq_hz` (1.85–2.78 GHz), `rf_hop_prepare` / `rf_hop`, `rf_pll_locked`, `rf_check_lock`, crystal correction `rf_set_xtal_ppb`, `rf_set_xtal_temp_model` + `rf_xtal_track`, AFC `rf_rx_tone_hz` + `rf_afc_step` | see "Synthesizer" below |
| Receive | `rf_rx_start`, `rf_rx_set_gain` (0..127 or AGC), `rf_rx_set_bandwidth_khz`, `rf_rx_capture`, `rf_rx_stream`, `rf_rx_power` | 80/40/16 MS/s; capture ≤ 16383 words; stream gapless |
| Transmit | `rf_tx_start`, `rf_tx_set_level_db` (−25..0), `rf_tx_play` (≤ 16384 words, 40 MS/s, loop), `rf_tx_tone_khz`, `rf_tx_stop` | the level mapping is approximate (−10 requested gives −8.9 measured); the calibrated table comes with the TX-chain work |
| Samples | `rf_sample_i`, `rf_sample_q`, `rf_sample_word` | 10-bit signed I/Q; `rf_sample_q` corrects the inverted raw Q |

## Test
`examples/rf_api_test` on C9:48, with D0 running `rfctl`, driven by `tools/rf_api_test.py`.

All checks pass:
- RX sees D0's tone 42 dB above the floor.
- D0 sees our tone at 43 dB.
- TX out of band is denied.
- Hop takes 38 µs.

## Synthesizer (section 1 of the RF plan)

Data: `bench/rfmeas/lock_range_fine.txt`, `freq_resolution.txt`, `hop_fast.txt`, `pll_loop_sweep.txt`, `xtal_tempco.csv`, `rf_afc.txt`.

**1.1 Range** (lock at 100 kHz steps, confirmed over the air):

| | C9:48 | D0 |
|---|---|---|
| Direct VCO, cold | 2207.6–2784.5 MHz | 2212.9–2788.3 MHz |
| Direct VCO, 90 °C | — | 2211.1–2785.5 MHz |
| 6/5 divider, lowest | 1839.5 MHz | 1842.6–1844.8 MHz |

- The API guarantees 1850–2780 MHz.
- Over the air, both RX and TX work at 1900, 2000, 2230, 2700 and 2780 MHz, 31–58 dB above the floor. The level falls away from 2.4 GHz (antenna and matching).

**1.2 Resolution:**
- One LSB of the 16-bit fraction: 30 MHz / 65536 = 457.76 Hz direct, 381.47 Hz through the divider. `rf_set_freq_hz` rounds to it.
- Measured with paired differences: one LSB is 478 ± 18 Hz, and +200 Hz rounds to no change (−26 ± 21 Hz).

**1.3 TX range:** the transmitter works through the divider too (above), so TX spans 1850–2780 MHz. The default limits still fence TX to 2400–2483.5 MHz.

**1.4 Hop:**
- `rf_hop` writes only the bytes that change, skips the latch (measured unnecessary), runs from IRAM and uses a precomputed N (the 64-bit division costs ~16 µs on this core).
- Results: 12.9 µs with 3 bus writes, 16.4 µs when the integer N changes; 14 µs through the API. The LO is settled in the first 3.2 µs block after return.
- Floor: 3.44 µs per analog-bus transaction. Its clock is not in 0x6000E050–E060; all fields there were tried.

**1.5 Loop settings:**
- 62.03 high nibble (0x7) and 63.01 low bits (0x3) are essential: other values kill the LO or lose lock. They are left at the vendor values.
- The candidate loop fields (62.03 low, 62.04 high, 62.08/09/0A high) show no effect above our measurement floor (about −49 dBc in 7.3 kHz, flat with offset, so the floor rather than the PLL).
- Characterising the loop needs a phase-noise analyzer (section 10).

**1.6 Frequency correction:**
- `rf_set_xtal_ppb` retunes the fraction with the cap kept (no recalibration).
- The open-loop temperature model (lagged die temperature, quadratic) is limited to about ±0.4 ppm: the die sensor leads the crystal, and transmit-on transients pull the crystal in ways no temperature model sees. Two heating runs fit with residuals of 80 and 440 ppb.
- **Closed loop (AFC) works:** `examples/rf_afc` locks C9:48 to D0's tone within 2 s, then holds a mean residual of +8 Hz (sd 195 Hz = ±1 LO step, 0.19 ppm) for 3+ minutes while D0 warms up and drifts by about 0.9 ppm.

**1.7 Lock:**
- `rf_pll_locked` reads the VCO control-voltage comparator; `rf_check_lock` re-tunes once.
- Tested: a forced bad cap is detected and recovered (`rf_api_test`). No loss-of-lock interrupt was found, so it is polled.
- `rf_set_freq_hz` retries a failed calibration once (one failure seen in about 1000 tunes).
