# RF API (`components/include/rf.h`)

The public interface to the ESP32-S3 radio in esp-ndf. No vendor blobs. Every call checks the state and its arguments and returns an `rf_err_t`. The bench-level drivers in `components/drivers/rf/` remain available, but they bypass these checks.

## States
`rf_open()` → IDLE → `rf_rx_start()` → RX, or `rf_tx_start()` → TX; `rf_tx_stop()` → IDLE; `rf_close()` → OFF.

## Calls

| Area | Calls | Notes (measured) |
|---|---|---|
| Lifecycle | `rf_open`, `rf_close`, `rf_state` | `rf_open`: init replay, LO at 2437 MHz, RX DC calibration for all 128 gain rows |
| Limits | `rf_set_limits`, `rf_get_limits` | TX is denied outside 2400.0–2483.5 MHz or above 0 dB unless the limits are widened by the caller |
| Synthesizer | `rf_set_freq_khz` / `rf_set_freq_hz` (1.85–2.78 GHz), `rf_hop_prepare` / `rf_hop`, `rf_pll_locked`, `rf_check_lock`, crystal correction `rf_set_xtal_ppb`, `rf_set_xtal_temp_model` + `rf_xtal_track`, AFC `rf_rx_tone_hz` + `rf_afc_step` | see "Synthesizer" below |
| Receive | `rf_rx_start`, `rf_rx_set_gain` (0..127 or AGC), `rf_rx_set_bandwidth_khz`, `rf_rx_capture`, `rf_rx_capture_dec`, `rf_rx_stream`, `rf_rx_power` | 80/40/16 MS/s raw (capture ≤ 16383 words); 2 / 1 MS/s channel-filtered and gapless into any buffer (`rf_rx_capture_dec`); stream gapless |
| Transmit | `rf_tx_start`, `rf_tx_set_level_db` (−25..0), `rf_tx_play` (≤ 16384 words, 40 MS/s, loop), `rf_tx_tone_khz`, `rf_tx_stop` | the level mapping is approximate (−10 requested gives −8.9 measured); the calibrated table comes with the TX-chain work |
| Samples | `rf_sample_i`, `rf_sample_q`, `rf_sample_word` | 10-bit signed I/Q; `rf_sample_q` corrects the inverted raw Q |

## Test
`components/drivers/rf/test_apps/rf_api_test` on C9:48, with D0 running `rfctl`, driven by `tools/rf/rf_api_test.py`.

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
- **Closed loop (AFC) works:** `examples/rf_lab/rf_afc` locks C9:48 to D0's tone within 2 s, then holds a mean residual of +8 Hz (sd 195 Hz = ±1 LO step, 0.19 ppm) for 3+ minutes while D0 warms up and drifts by about 0.9 ppm.

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
| Coded rates | Same hardware path as 1M/2M, data exact. Cause of the old 0–97 % per-run catch rate: a coded receive window that ends without sync (status 0x01) while the AGC runs leaves the link-layer core unable to sync on any rate until a core reset. Events keep ending; the AGC, modem and core words, rf_open, ble_hw_init and the watchdog abort do not clear it; SYSCON 0x60026018 bit 9 (RW_BTMAC_RST) + modem/core reload does (`bench/rfmeas/coded_*.py`, `data/coded_*.txt`). Held gain or the forced front end do not enter the state. `rf_pkt_read` now does that reset and re-arms after each such timeout (`ble_hw_core_reset`). Catch rate is limited to every second back-to-back packet by the re-arm time, as on the forced path. |
| Receive re-arm | Event kick lead 200 µs on receive (250 µs leaves a gap: 908/1000; ≤ 150 µs is never picked up), 250 µs on send. Counted 1000 × 100 B: 1000 received (bench/rfmeas/rx_lead.txt). |
| Global switches (RWBLECNTL) | Bit 9 = advertising filter, which caps at 37 bytes; turned off. Bit 13 = CRC off, on both sides. Bit 19 on the transmitter = header byte sent exactly as given. Bit 14 = whitening off (header, length and payload literal; on one end only, the frame equals the BLE whitening sequence for the channel XOR the literal bytes, exact: `bench/rfmeas/whiten_b14.txt`). Bits 17/18 (CRC/whitening on other RW cores) do nothing here. |
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
- **Whitening off.** RWBLECNTL bit 14 (`RF_PKT_WHITEN_OFF`). Without whitening, low-transition payloads take bit errors (0,1,2.. test payload: 743/1000 packets with a wrong byte at −64 dBm); use it with DC-balanced data or for chip-level capture.

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

## 2.2 Raw-IQ gain control (`rf_rx_set_gain_db`, `rf_rx_autogain`)

The baseband AGC does not act on raw IQ. With it enabled, the gain register stays at 0x32 and the capture clips at every input level (bench/rfmeas/rx_agc.txt); Wi-Fi's AGC only acts on its own packets.

`rf_rx_set_gain_db(0..45)` picks a gain-memory index from the measured map. Steps are 1 dB ± 0.2, except one front-end switch between 33 and 37 dB. `rf_rx_autogain(target, &db)` captures, compares the peak with the target and steps the gain by the difference.

Measured with a CW at amplitudes 300 down to 3 (a 40 dB range), target 256:

| Measure | Result |
|---|---|
| Final peak | 244..261 |
| Time | 670..893 µs |
| No signal | gain 45, peak 15: the top of the map stays clear of noise clipping |

## 2.3–2.6 Rest of the RX chain

- **2.3 filter:** `rf_rx_set_bandwidth_khz` (measured corners, §A.2). The analog filter cannot go below a ~7 MHz corner: every block-0x67 register, alone and with the tightest code, was swept (`bench/rfmeas/rx_filt_sweep.py`, `data/rx_filt_sweep*.csv`); codes above 0x3F alias to 0x00–0x3F, and the BLE receive state uses the same 0x1F (the BT path filters digitally in the modem). Narrow channels are therefore filtered in software: **`rf_rx_capture_dec(decim, n, dst)`** runs the 16 MS/s stream through a Hamming FIR and decimates by 8 (2 MS/s, 48 taps, −3 dB at ±0.6 MHz, −23 dB at 1.1 MHz, −50 dB at 1.3 MHz) or 16 (1 MS/s, 128 taps, −3 dB at ±0.3 MHz, −22 dB at 0.5 MHz, −52 dB at 0.65 MHz). Synthetic tones through the firmware match the theoretical response within 1 dB down to −50 dB; the floor is −55…−78 dB (10-bit input, Q15 taps, int16 output). Over the air at 2 MS/s the response matches to −50 dB with a −58…−62 dB measurement floor (`data/rx_dec_resp.txt`, `data/rx_dec_syn.txt`).
- **2.3b lower sample rates:** `rf_rx_capture_dec` is gapless at both rates into internal RAM or PSRAM (int16 I/Q, 4 B/sample): 1 000 000 samples = 0.5 s at 2 MS/s / 1 s at 1 MS/s, worst stream lateness 1 µs (`data/rx_dec_long.txt`). Cost 11.8 / 11.1 CPU cycles per input sample of the 15 available (PIE unpack `dsp_unpack_iq10` 2.1 cycles, fused I+Q dot `dsp_dot2_s16`). 4 MS/s (decim 4) is not offered: its windows are not 16-byte aligned for the vector loads.
- **2.4 IQ balance** (`bench/rfmeas/rx_image_sep.py`, `rx_iq_*.py`, `data/rx_i*.txt`): the old 30.3–41.3 dB (bench/rfmeas/rx_image.txt) included D0's TX image in the same bin. With the RX LO 2 MHz below the TX LO (tone at +3 MHz, our image at −3, D0's TX image at +1) and the gain held for a good SNR, image rejection is **45–62 dB** across 2404–2478 MHz. The RX IQ word 0x6000607C [26:16] does not act on the capture (confirmed). A software correction y = x − β·conj(x) does not help: β is not stable between captures (|β| ≈ 0.003, angle −63° … −132°), and β averaged over 4 captures gains 0–2 dB at +3 MHz and loses up to 17 dB at +5 MHz. The residual is random content in the image bin, not a static imbalance, so no RX IQ calibration is applied.
- **2.5 sample formats:** 80 / 40 / 16 MS/s, 10-bit signed I and Q per 32-bit word (`rf_sample_i/q`); gapless streaming at all three rates (§A.4).
- **2.6 saturation:** the ADC clips at ±512. The datasheet maximum RX input is +5 dBm (Table 6-5); damage was not tested. `rf_rx_autogain` keeps the peak under the clip.

## 3.1 TX level (`rf_tx_set_level_db`)

- **What the level controls.** The level is the digital gain byte (0x60006014..30). Its response was measured against the maximum code 0xF8 taken right before each code, because the chip's output drifts about +1 dB over 2 minutes of continuous tone (bench/rfmeas/tx_drift.txt) and a one-pass sweep is biased by that.
- **Accuracy.** From the drift-proof table (bench/rfmeas/tx_dig_sweep2.txt), checked the same way in shuffled order, every level from 0 to −25 dB is within 0.64 dB (bench/rfmeas/tx_level.txt). The earlier linear map was off by up to 2 dB.
- **Code range.** 0xF8 is the maximum, not 0xF0. Codes ≥ 0xFC and positive values fall back to lower output.
- **Packet engine.** It uses the core's power index instead; see the table in §4.1.

## 3.2–3.6 Rest of the TX chain

**3.3 TX LO leakage and image** (bench/rfmeas/tx_lo_leak.txt). The receiver sits 2 MHz below D0's LO, so the image, the leak and the tone land at +1, +2 and +3 MHz.

| | Carrier, full level | Image |
|---|---|---|
| Uncalibrated | −18.7 dBc | −34..−40 dBc |
| With `rf_tx_dc_cal` | −28.4 dBc (−27.1 / −29.2 at 2402 / 2480 MHz from one calibration at 2440) | −40 dBc |

- `rf_tx_start` now runs the calibration once per `rf_open`, in about 56 ms. It stops the DAC when it finishes, so it must run before any playback.
- The leak is a fixed absolute level, so with digital level steps it rises relative to the tone: about −13 dBc at −10 dB and −2 dBc at −20 dB.
- **No analog attenuator on the DAC path** (bench/rfmeas/tx_6b_*.txt, tx_pbus_gain.txt):
  - PHY-bus PA / row 4 words have no effect while the hardware owns the front end.
  - 6B.02 and 6B.04 move the output by only about 6 dB.
  - 6B.04 = 0x0B..0x0F lowers the carrier more than the tone (0x0D: tone −3.4, carrier −10.4 dB), an optional extra 7 dB of carrier suppression at −3.4 dB of output.
- The packet engine's power index works on the core's TX gain rows: about 40 dB of range (§4.1).

**3.2 TX state machine.** pbus-memory word 42 bits 7:4 are the BT TX state's PA enable, used to make the packet receiver silent (§4.1). Word 19 is a Wi-Fi state word; clearing word 24 kills receive.

**3.4 DAC rates.** 40 MS/s (loop playback and gapless streaming) and 80 MS/s (`rf_dac_play` rate80).

**3.5 Tone.** `rf_tx_tone_khz`: DAC-generated CW at ±19 MHz, amplitude 1..511.

**3.6 Power detector.** `rf_pwdet_on` / `rf_pwdet_read` (SAR2, 8 × 13-bit results); `rf_tx_dc_cal` uses it.

## 5. Timing (`rf_time_us`, `rf_pkt_send_at`, `rf_pkt_t.time_us`)

- **Time base:** the packet hardware's own clock (half-slot counter + fine counter, 0.5 µs resolution), on the board's crystal.
- **Scheduled send:** `rf_pkt_send_at` puts one packet's start on that clock, at least 300 µs ahead (an earlier time returns RF_ERR_TIMEOUT).
- **Receive timestamps:** received packets carry the sync-word time.

Measured (bench/rfmeas/sched_timing.txt). D0 schedules packets at t0 + k·period; C9:48 timestamps each on its own clock:

| Period | Sent / late | Received | Intervals (after the first) |
|---|---|---|---|
| 10 000 µs | 300 / 0 | 300 | 9 999..10 001 µs |
| 2 000 µs | 500 / 0 | 500 | 1 999..2 001 µs |
| 1 000 µs | 500 / 0 | 500 | 999..1 001 µs |

- **Accuracy:** scheduling plus timestamping is within ±1 µs, the resolution of the timestamp.
- **Clock offset:** the two crystals differ by about 2 ppm (5 kHz at 2.46 GHz, bench/rfmeas/board_freq_offset.txt), 20 ns per 10 ms.
- **Turnaround:** RX → TX on one board is bounded by the 300 µs schedule lead after the packet ends.

## 8. Calibration store

`rf_open` loads this chip's stored calibration when it can, instead of recalibrating:

- **What is stored:** RX DC words for all 128 gain rows, the TX DC table and the crystal correction, as one CRC-checked record in the `phy` data partition (flash the partition table with `make partition`), tagged with the MAC and the die temperature. It is not kept in NVS, whose 16 KB RAM index would crowd out RF apps.
- **When it is loaded:** when the MAC matches and the temperature is within 15 °C. Otherwise `rf_open` calibrates and stores.
- **TX DC:** the first `rf_tx_start` runs the TX DC calibration and stores it.
- **Temperature trigger:** `rf_cal_check()` recalibrates and stores when the die has moved more than 15 °C.

Measured on D0 with `tools/rf/rf_cal_test.py` (bench/rfmeas/rf_cal_test.txt), 6/6 pass:

| | Cold (calibrate + store) | Warm (load) |
|---|---|---|
| `rf_open` | 46 ms | **6 ms** |
| First `rf_tx_start` | 86 ms | **0 ms** |
| TX carrier | −27.7 dBc | −27.8 dBc |
| RX DC residual, gains 0..127 | ≤ 2.0 LSB | ≤ 1.8 LSB |

## 9. Safety

- **Band.** `rf_set_limits` sets `tx_min_khz..tx_max_khz`; the default is 2400–2483.5 MHz. Every TX entry point refuses frequencies outside it: `rf_tx_start`, `rf_pkt_send`, `rf_pkt_send_at`.
- **Level.** `tx_max_db` (default 0 dB) is checked against `rf_tx_set_level_db` and against the packet power index (measured table).
- **Time.** `tx_max_ms` (new, default 0 = off) bounds a continuous transmission (`rf_tx_start` .. `rf_tx_stop`, or a continuous `rf_pkt_send`). A timer switches the transmitter off and the state returns to IDLE, with `rf_tx_expired()` = 1.
- **Recovery.** `rf_check_lock` re-tunes after a lost PLL lock (§1).
- **Damage.** Not tested; the datasheet maximum RX input is +5 dBm.

Measured (bench/rfmeas/rf_safety.txt):

| Test | Result |
|---|---|
| Tone, `tx_max_ms` = 500 | gone after 1 s (35.9 → −25.3 dB), state IDLE, expired 1 |
| Tone, no limit | still on after 1 s (35.5 dB) |
| 50 cycles of `rf_open`, hop across the band, TX tone, lock check, `rf_close` | 0 failures |

## 6.1 PSRAM (`components/drivers/psram`, `components/include/psram.h`)

- **Device:** 8 MB AP Memory octal PSRAM in the ESP32-S3R8 package, on MSPI CS1, driven by our own code.
- **Bring-up:**
  1. **Pins:** the flash pads GPIO27–32 (shared CLK and D0–D3) are switched from the GPIO matrix to MSPI function 0, and GPIO33–37 (D4–D7, DQS) to function 4.
  2. **Clocks:** MSPI core clock 160 MHz, with IDF's input timing.
  3. **PSRAM registers:** read and written over SPI1 in octal DTR (16-bit commands 0x4040 / 0xC0C0, 32-bit address, 8 dummy), then MR0 is set to fixed latency, read latency code 2.
  4. **Cache path:** SPI0 commands 0x0000 / 0x8080, read dummy 18, write dummy 8.
  5. **Mapping:** all 128 pages at 0x3D000000.
- **What was missing:** with those flash pads left on the GPIO matrix (as after ROM boot), the PSRAM never answers. Found by diffing all MSPI / IO MUX registers against IDF (bench/idf_psram/state_diff.txt) and by reading the ROM's own `esp_rom_opiflash_exec_cmd` (dumped from the chip).
- **Speed:** see PERFORMANCE.md (49.5 / 84.6 MB/s, ahead of IDF).

## 6.2 Long and triggered captures (`rf_rx_capture_to`, `rf_rx_capture_trigger`)

Both run on `rf_rx_stream`, so they are gapless. The destination can be any memory, including PSRAM. `RF_FMT_IQ8` packs I and Q to 8 bits each (2 B/sample).

Measured with C9:48 capturing D0's tone (bench/rfmeas/psram_capture.txt):

| Capture | Result |
|---|---|
| 16 MS/s, IQ8, 4 194 304 samples into PSRAM (all 8 MB) | **gapless: 262.1 ms of air in 263.1 ms**; tone present at start / middle / end (13 dB), 35–45 dB above an empty bin |
| 16 MS/s, IQ32 into PSRAM (64 MB/s) | too fast for PSRAM writes (49.5 MB/s): refused with RF_ERR_TIMEOUT after 2 units |
| 40 MS/s, IQ8 into PSRAM (80 MB/s) | too fast: RF_ERR_TIMEOUT |
| Triggered, 16 MS/s, 400 000-sample ring, 100 000 pre-trigger | fires; the ring and pre-trigger indices are consistent (trigger − first ≈ pre) |

**Limits**

- **Trigger granularity:** the level is checked on every 16th sample (1 µs at 16 MS/s), so the scan fits the stream's time budget. Spikes shorter than that pass unseen. In this room, ambient 2.4 GHz bursts and short near-full-scale spikes crossed level 90 before the test tone started.
- **IQ32 at 16 MS/s and anything at 40/80 MS/s** fit internal RAM only (`rf_rx_stream` / `rf_rx_capture`).
- **DSP helpers:** `components/include/dsp.h` (FFT, filters); `rf_rx_tone_hz` estimates a tone's frequency.

## 4.2 Raw 802.11 over the Wi-Fi MAC (`rf_wifi_tx`, `rf_wifi_rx`)

`components/drivers/rf/rf_wifi.c`, example `examples/rf/rf_wifi`, host driver `tools/rf/rf_wifi_test.py`.
Builds and sends / receives 802.11 frames directly through the Wi-Fi MAC descriptor
path on channel 6 (2437 MHz) — our own code, no vendor Wi-Fi stack.

| Call | What it does |
|------|--------------|
| `rf_wifi_tx_init()` | carrier on ch 6 + TX front end (calibrated RF path), replay the captured absolute baseband/FE/MAC transmit state |
| `rf_wifi_tx(frame, len)` | DMA one frame (10..1600 B) through the MAC; `RF_OK` when the TX state machine completes |
| `rf_wifi_rx_init()` | carrier on ch 6 + RX front end, replay the RX state, arm our own 4-descriptor ring |
| `rf_wifi_rx(out, max, timeout_ms)` | next received frame into `out` (control header stripped); returns length, 0 on timeout |

The absolute register state (`components/drivers/rf/rf_wifi_regs.h`) was captured from a reference
transmit/receive (`bench/idf_phyble`, `bench/idf_wifirx`); both the digital config **and**
the calibrated carrier/front end are required — digital config alone transmits nothing.

**Measured** (two boards, shielded enclosure; `bench/rfmeas/wifi_tx_42.txt`, `wifi_rx_42.txt`):
one board `rf_wifi_tx` continuously, the other `rf_wifi_rx` — 5906/5906 received frames were
ours (len 100, rssi ~−46 dBm, ~984/s), addr2 = the lab source id `02:4e:44:46:00:01`.
Descriptor: TX word0 `0xC0000000|len<<12|len`; RX word0/word3 `0x80640000|bufsize` (owner bit 31),
the MAC clears the owner and writes the length when a frame lands. RX control header is 48 B.

## 4.3 802.15.4 O-QPSK by DAC waveform (`rf_154_build`)

`components/drivers/rf/rf_154.c`, example `examples/rf/rf_154`, host `tools/rf/rf_154_test.py`.
The S3 has no 802.15.4 radio, so `rf_154_build(psdu, len, out, max)` synthesises the
O-QPSK/DSSS baseband at 40 MS/s (2 Mchip/s, 32-chip PN per 4-bit symbol, half-sine pulse,
Q offset half a chip) for preamble+SFD+PHR+PSDU; play it with `rf_tx_play` (looped) on
channel 11 (2405 MHz). Transmit only.

**Verified** (`bench/rfmeas/wifi_154_43.txt`): captured on the sibling board at 16 MS/s,
matched-filtered, chips correlated against the PN table — a run of 8 identical symbols at
full 32/32 chip correlation = the preamble (4 zero octets), i.e. a valid O-QPSK/DSSS signal.

## 4.4 Correlator trigger (`rf_corr_trigger`)

`components/drivers/rf/rf_corr.c`. `rf_corr_trigger(iq, n, refI, refQ, rlen, &off, &mag)` slides a
reference complex waveform over a captured IQ window and returns the sync alignment (trigger
sample `off`) and the peak-to-mean power ratio (×256). Triggers on a known sync *pattern*,
unlike the energy-level trigger `rf_rx_capture_trigger`. The `rf_pkt` `corr` command
correlates a capture against a 4-symbol 802.15.4 preamble: ratio 30 with the signal present
vs 17.6 silent, with a defined trigger offset (`bench/rfmeas/corr_trigger_44.txt`).
