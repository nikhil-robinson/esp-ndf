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

## 3.4 Gapless TX streaming (`rf_tx_stream`)

The DAC loops a 16384-word ring at 40 MS/s. The CPU cannot rewrite the bank the DAC is playing, and even reading it disturbs the output. Banks 0 and 1 therefore alternate:

- **Units:** unit k (8192 words, 205 µs) plays from bank k%2, in ring half k%2.
- **Tail copy:** the next unit goes into the free bank, together with a copy of the playing unit's last 2048 words. This tail is kept in bank 2's memory; bank 2 is never selected. The bank switch can then happen anywhere in that 51 µs tail without a seam.
- **Position tracking:** the DAC position comes from the CPU cycle counter (6 cycles per sample, same crystal).

Measured (bench/rfmeas/tx_stream.txt, rf_api_test.txt; D0 transmits, C9:48 receives):

| What | Result |
|---|---|
| Ring position sensor: a parallel capture, or selecting bank 2 | stops the DAC (tone 34.5 dB → −27.6 dB) |
| Bank switch between identical banks | seamless (30/30 captures clean, ≤ 0.26 rad) |
| 48,000 units (9.8 s) of a 2.5 MHz tone | 0 late switches, 0 / 100 captures with a phase jump > 0.5 rad (worst 0.098 rad) |
| Budget per unit | 153 µs window, of which the tail copy takes 76 µs; about 75 µs is left for the callback (a 16-word table fill takes 45 µs) |

## 4.1 Hardware packet engine (`rf_pkt_*`)

Packets are framed, CRC'd, whitened and stored by the radio's packet hardware, with no CPU in the data path and no BLE protocol in our code.

**Background.** The demodulator can only be switched on by the link-layer core. Step 0 checked this: single-bit scans of the modem, coex, sequencer and core registers found no other enable (bench/rfmeas/modem_bitscan.txt). Espressif's own lowest-level radio test (libphy `esp_phy_ble_tx`/`rx`) also runs on that core in a protocol-free test mode (bench/idf_phyble). The core is therefore used only as a framer/deframer.

**How the engine is built**

| Part | Setting (each one measured) |
|---|---|
| Send | Connection-master control-structure format 0x02 with no connection. One packet per event, re-kicked from the end interrupt. CS+0x18 holds the TX power only, because the template's bits 15:13 block transmit. |
| Receive | Connection-slave format 0x03; this is the only format found that takes packets up to 255 bytes. Max length 0xFF at CS+0x28, one packet per event, two of our own 256-byte buffers. |
| Receive-only | The slave format always answers (D0 hears 574 replies); neither control-structure bits nor the TX power index stop it. pbus-memory word 42 (bits 7:4) is the BT TX state's PA enable: cleared during receive, every reply stops (D0 sends at its full rate and is never acknowledged) while the hardware receive sequence and AGC keep working (bench/rfmeas/pbus_pa_hunt.txt). Word 24 kills receive; word 19 is not the BT PA. |
| Coded rates | Hardware-sequenced receive of coded packets is erratic (0..81 % per run). Coded uses the front end forced to receive (gain code 127, TX path off): it works only near the level that code suits (41 % / 50 % at the 2026-10-22 bench geometry). Proper gain selection is RX-chain work. |
| Global switches (RWBLECNTL) | Bit 9 = advertising filter, which caps at 37 bytes; turned off. Bit 13 = CRC off, on both sides. Bit 19 on the transmitter = header byte sent exactly as given. Bits 17/18 (CRC/whitening on other RW cores) do nothing here. Bit 14 scrambles part of the frame and is not a clean whitening switch, so whitening-off is refused for now. |
| Test-mode formats (libphy) | 0x1C TX test sends only built-in patterns (PRBS9, 0x55, 0xF0). 0x0A RX test caps at 37 bytes. |

**Measured** (bench/rfmeas/rf_pkt_test_final.txt; D0 sends, C9:48 receives):

| Case | Result |
|---|---|
| 1M / 2M / coded S8 / coded S2 | data exact, 0 wrong bytes |
| 2402 / 2404 / 2426 / 2440 / 2478 / 2480 MHz | data exact |
| Lengths 1 / 37 / 100 / 200 / 255 bytes | data exact |
| Sync words 0x71764129, 0x8E89BED6, 0x12345678, 0xA5A5F00F | all decode with exact data |
| Counted send, exactly 1,000 × 37 bytes | 1,000 received |
| 1,000 × 100 bytes | 1,000 received |
| 1,000 × 200 bytes | 999 received |
| CRC off on TX, CRC on at RX | every packet CRC-bad with exact data |
| CRC off on both sides, or CRC off at RX only | good |
| Header byte 0x5A | received as 0x5A |
| Out of band (2500 MHz) / outside `rf_set_limits` | refused |
| CPU while sending | not measurable (one short interrupt per packet); 0 while receiving apart from reading packets |
| Send rate, 37 bytes at 1M | 1,180 packets/s |
| Receive rate | about 1,000 short packets/s (the re-arm takes about 0.5 ms); every packet of 100 bytes or more |

**Receive quality, hardware AGC, 1M (2026-10-22, boards re-plugged, RSSI −45 dBm)** (bench/rfmeas/rx_vendor_regs.txt):

| Frequency / length | Received of 1000 | CRC-bad |
|---|---|---|
| 2440 MHz / 37 B | 922 | 3 |
| 2440 / 200 | 880 | 19 |
| 2420 / 200 | 899 | 18 |
| 2402 / 200 | 873 | 14 |
| 2460 / 37 and 200 | 724 / 758 | 103 / 115 |

The forced front end (fixed gain code) was 1000/1000 at RSSI −20 and fails at −33: a fixed gain is not usable. The 46 configuration words that differ from the vendor's BLE scanning state were each tried at −64 dBm (no improvement); the vendor test transmitter is no reference (DTM packets are not whitened). Lower TX power makes reception worse (−64 dBm: ~0 %), so this is a sensitivity problem of our receive set-up, not overload. Our earlier hardware BLE scanner received 91 % at this distance too.

**Sensitivity against IDF (IDF-sibling rule)** (bench/rfmeas/sens_idf_vs_ours.txt). D0 sends our hardware adverts on channel 37, 500/s, at TX power index p. C9:48 receives them first with IDF's Bluedroid scanner (rotating over all 3 channels, so the result is normalised to its 1/3 share), then with our `rf_pkt` receiver:

| Power index | IDF scanner | ours |
|---|---|---|
| 11 | 100 % (IDF RSSI −66) | 100 % (7 CRC-bad of 1078) |
| 8 | 100 % (−69) | 100 % |
| 5 | 10 % (−83) | 25 % |
| 3 / 1 | 0 % | only other devices' adverts |

Conclusions:
- **Sensitivity matches or beats the vendor stack.** The losses at low power come from the transmitter at power indexes ≤ 5, which are very weak; they are not a fault in our receiver.
- **Long packets lose more at the same level.** 200 bytes give 2–15 % CRC errors at index 11 depending on frequency, which is expected, because packet error rate grows with length.
- **Settings that changed nothing.** None of the 46 differing configuration words, nor the vendor analog groups for the PLL loop, RX filter or blocks 61/66/6D, changed reception on either side.
- **Block 0x6B is a TX output block.** The vendor BT values raise our TX output by about 7 dB; this goes to section 3, TX chain.

**Open items**
- **Whitening off.** No clean switch found yet.

## 2.1 RX gain stages

Measured by sweeping all 128 gain-memory indices with a CW from D0, at full level and 30 dB down (bench/rfmeas/rx_gain_curve*.txt, parsed in rx_gain_table.csv).

**How gain memory is organised**

- Indices 0..82 are the Wi-Fi RX gain table. Indices 83..127 are a second, shorter table, the BT table; that is why the packet engine tops out at index 127.
- **Front-end code** (low word bits 31:24) selects the LNA/mixer setting. Gain relative to code 0x40, at baseband code 0, from the unclipped points:

  | 0x40 | 0x08 | 0x0C | 0xC4 | 0x1C | 0xD0 | 0xDC | 0xA0 | 0xA4 | 0xE4 / 0xF0 / 0xFC |
  |---|---|---|---|---|---|---|---|---|---|
  | 0 | +5.1 | +13.0 | +15.5 | +17.3 | +19.8 | +22.9 | +28.6 | about +37 | higher (clipped at this input) |

- **Baseband (VGA) code** (low word bits 23:16): 0x00..0x0A gives 0..5 dB in 1 dB steps (bit pattern ×2). 0x10 continues at +6 dB, 0x12..0x1A up to +11 dB.
- **Low word bits 15:0 and the high word:** per-entry DC correction and further stage bits. `rf_rx_dc_cal` writes the DC words.

**Saturation and noise**

- The 10-bit ADC clips at ±512.
- With the weak tone, the total 40 MHz power runs about 20 dB above the tone: noise plus the surrounding 2.4 GHz traffic.
- From Wi-Fi index 55 up, the ADC clips on that alone, with no signal needed.
- The BT table stops at index 127 (front end 0xA0, peak 190 on noise), just below clipping. That is a sensible ceiling for a 1 MHz channel.

**Forced gain in packet receive**

- The packet engine's forced gain codes (pbus row 1 col 2) are not these indices.
- Hardware AGC (`RF_GAIN_AGC`) is the default.
- RSSI is reported in dBm on IDF's scale: the core's raw value minus 22 dB, measured against IDF Bluedroid on the same adverts (−69 / −83 dBm vs raw −47 / −60).
