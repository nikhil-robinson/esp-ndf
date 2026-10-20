# RF API (`include/rf.h`)

The public interface to the ESP32-S3 radio in esp-ndf. No vendor blobs. Every call checks the state and its arguments and returns an `rf_err_t`. The bench-level drivers in `drivers/rf/` remain available, but they bypass these checks.

## States
`rf_open()` → IDLE → `rf_rx_start()` → RX, or `rf_tx_start()` → TX; `rf_tx_stop()` → IDLE; `rf_close()` → OFF.

## Calls

| Area | Calls | Notes (measured) |
|---|---|---|
| Lifecycle | `rf_open`, `rf_close`, `rf_state` | `rf_open`: init replay, LO at 2437 MHz, RX DC calibration for all 128 gain rows |
| Limits | `rf_set_limits`, `rf_get_limits` | TX is denied outside 2400.0–2483.5 MHz or above 0 dB unless the limits are widened by the caller |
| Synthesizer | `rf_set_freq_khz` (1.85–2.775 GHz), `rf_hop_prepare` / `rf_hop` | `rf_hop` takes 38 µs; the tone is at full level within 3.2 µs |
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
