# RF work — paused 2026-10-23

State of the RF work when it was paused, so it can be picked up again without re-deriving anything.
Main RF plan (sections 1–10) and its status: `docs/PENDING.md`. Measured results: `docs/PERFORMANCE.md`.
API and findings: `docs/RF_API.md`. Lab wording rule: plain engineering wording, two-board Faraday-cage bench.

## Read first: repository and bench state at the pause

- **Git (repaired 2026-10-24):** 18 object files were found empty after an interrupted write; the four RF commits
  of 2026-10-22/23 (whitening-off, coded-rate fix, RX image rejection, decimator) were lost as commits but not as
  files. `ndf-core` was reset to `origin/ndf-core` (`ed9644d`) and the working tree recommitted. Copy of the damaged
  `.git`: `/home/nikhil/esp-ndf-git-backup-2026-10-23`.
- **D0 offline.** Its CH340 (`/dev/ttyUSB1`, by-path `pci-0000:05:00.4-usb-0:1:1.0-port0`) disappeared mid-run
  and did not re-enumerate. Needs a replug before any two-board test. C9:48 on `/dev/ttyUSB0` is fine.
- **Boards' firmware:** C9:48 runs the current `examples/rf/rf_pkt` build. D0 last had the build with the `gfsk` command.
- **Uncommitted (in the working tree):** the `gfsk` generator in `examples/rf/rf_pkt/main.c`, `STACK0_SIZE = 7168`
  in `examples/rf/rf_pkt/app.mk`, the `corr` command sharing `sbuf`, `bench/rfmeas/demod_*.py` and
  `bench/rfmeas/data/demod_*.txt`, this file.
- **`rf_pkt` is at its RAM limit** (code + data + stacks must end below `0x3FCB0000`, the dump engine's banks).
  New bench probes go into a separate small app, not `rf_pkt`.
- **Pre-existing build failures** (same RAM check, already failing before this work): `components/lib/test_apps/libc_test`,
  `components/drivers/*/test_apps/*_unit`, `components/soc/test_apps/sys_test`.

## Follow-up list 1 (approved 2026-10-22) — done

| Item | Result | Where |
|---|---|---|
| Coded-rate (S2/S8) receive run-to-run 0–97 % | Fixed. A coded window that times out while the AGC runs leaves the link-layer core unable to sync on any rate until a core reset (SYSCON `0x60026018` bit 9, RW_BTMAC_RST) plus modem/core reload. `rf_pkt_read` now does that after each such timeout. S8 16–18 per 300 ms window over 20 windows; silence-first start 68–74/150 (was 0). | `ble_hw_core_reset`, `data/coded_*.txt` |
| Whitening disable | RWBLECNTL bit 14 = whitening off (header, length, payload literal). Proven: one-end-only frame = BLE whitening sequence XOR literal bytes, exact. `RF_PKT_WHITEN_OFF` works. Unwhitened low-transition payloads take bit errors. | `data/whiten_b14.txt` |
| Section 7 bench measurements | Waiting on the lab: supply current per state, BT hand-off with a live stack, antenna/PA switch control. Measurement list given in the session. | — |

## Follow-up list 2 (approved 2026-10-22) — where it stopped

| # | Item | State | Result / next step |
|---|---|---|---|
| 1 | RX IQ calibration | **Done** | The 30–41 dB figure included D0's TX image. Measured clean (RX LO 2 MHz below TX LO): 45–62 dB. RX IQ word `0x6000607C` does not act on the capture; a software correction gains 0–2 dB (β not stable between captures). No calibration applied. `data/rx_iq_*.txt` |
| 2 | Narrower receive filter | **Done** | Analog filter floor ~7 MHz (every block-0x67 register swept). Software channel filter `rf_rx_capture_dec`: decim 8 → 2 MS/s (48-tap, −3 dB ±0.6 MHz, −50 dB at 1.3 MHz), decim 16 → 1 MS/s (128-tap, −3 dB ±0.3 MHz). Matches theory to −50 dB. `data/rx_filt_sweep*.csv`, `data/rx_dec_*.txt` |
| 3 | Lower sample rates | **Done** | `rf_rx_capture_dec` gapless into RAM/PSRAM: 1 M samples (0.5 s at 2 MS/s, 1 s at 1 MS/s), late ≤ 1 µs. 11.8 / 11.1 cycles per input sample vs ESP-DSP `dsps_fird_s16` 149.5 / 134.8 (12–13×). New PIE kernels `dsp_unpack_iq10`, `dsp_dot2_s16`. 4 MS/s not offered (alignment). |
| 4 | Configurable hardware demodulator | **Done: closed** | `gfsk` DAC generator (any rate/deviation, engine frame format) validated: 1M/250 kHz and 2M/500 kHz decode with CRC. Default tolerance at 1M: deviation 200–500 kHz, rate within ±2 % (980/1020 kbit/s: sync, CRC fails). Single bits of the modem block and `0x6001CC00` block: nothing makes 100 kHz deviation decode. All 32 combinations of modem `0x6C[27:24]` and `0x70` bit 4 per engine rate (`demod_modes.py`, 2026-11-01): no effect: 1M decodes only on engine rate 0 and 2M only on 1 (167–176 of ~176 for every combination), 500 and 250 kbit/s never. Whole words that differ between the four engine rates (`demod_ccdiff.py`, `data/demod_ccdiff.txt`: 4 modem words, 9 in `0x6001CCxx`, which change run to run like estimator state) forced one at a time and 8 together: 1M at 100 kHz still 0 good / ~220 CRC-bad. The demodulator is the four engine rates with a fixed tolerance; no rate or deviation field is reachable. Other modulations: capture IQ (`rf_rx_capture_dec`). |
| 5 | Receive while transmitting | **Done: not possible** | `examples/rf/rf_selfcap` (one board, `tools/rf_selfcap.py`, `data/selfcap.txt`). While the transmitter is on (DAC tone, hardware tone generator, packet engine) the capture is one constant word for all 8000 samples: I = -484 at gain 0, -507 (clipped) at gain 90. The ADC and gain stages run (the constant follows the gain), but the RF input is cut: only the chain's DC offset remains, no tone at any offset or gain. Forcing the receive front end on (`rf_rx_on`, `rf_pbus_xpd_rx_on`) does not reconnect it. With the DAC playing, capture and DAC share the address counter (capture times out). Packet engine: samples are live between packets and freeze during each packet. So receive and transmit are time-shared on this chip: listen in the gaps (turnaround figures in section 5). |
| 6 | Absolute transmit power | Lab | One calibrated power reading at index 15 / 0 dB; the relative table then gives dBm. |

## Open items carried over

- Receive RSSI offset −22 dB relative to IDF (applied as a constant).
- Transmit output drifts about +1 dB over 2 minutes.
- 1M long packets at 2460/2480 MHz at −71/−74 dBm fail the 5 % CRC limit in `rf_pkt_test` (sensitivity margin; failing in the baseline too).

## Wi-Fi 40 MHz (HT40) receive — open, 2026-11-01

Shipped instead: we advertise 20 MHz only (HT Capabilities 0x012c), so an HT40 AP serves us at 20 MHz (docs/PERFORMANCE.md).
What is known about IDF's HT40 station state (same board, BOX-3, `bench/wifi_regs/diff_idf_sta.py`):
- LO at the 40 MHz centre, channel + 10 MHz: regi2c 0x63 words 3..5 equal ours at 2447 MHz; 0x62 words 1/5 are the VCO cap (per chip).
- Stable HT40-vs-HT20 differences besides the LO: FE 0x60006100 / 0x60006124, BB 0x6001cc10 / cc14 / cc48 / ccb0 / ccd0 bit 29,
  0x6001d014 / d08c, MAC 0x60033400 bits 1:0, 0x60034268 bit 7 (`data/idf_box3_ht40_vs_ht20*.json`). The first ten are already in
  `rf_wifi.c` `ht_rx[]` (that capture was against the default HT40 AP), and 20 MHz reception works with them.
- Tried (scripts `bench/wifi_regs/ht40_*.py`): LO + 10 MHz with our own cap, plus FE 0x600060c0, 0x60034268 bit 7, 0x6001ccd0 bit 29:
  our broadcast probe is heard by the AP, but 20 MHz frames 10 MHz below the LO mostly fail to decode (frames per 3 s: 45-56 vs 335 at
  20 MHz; 0 of 50 unicast frames); authentication fails. Adding IDF's whole HT40 FE/BB state (31 words) or IDF's regi2c blocks
  0x64/0x69/0x6a/0x6b/0x6d: no gain. The analog filter (0x67) is the same in IDF's two modes and within one code of ours.
- Next step: read the HT40 path in libphy (`chip_v7_set_chan`, `chip_v7_set_chan_offset`, `chip_v7_set_chan_misc`, `phy_chan_filt_set`
  writes 0x6001cd04/cd08 bit 28 + 0x6001c074, `set_channel_rfpll_freq`) to find what our captures miss (registers that only exist while
  receiving, or writes outside the captured ranges). Expected gain on this bench: none (at 20 MHz we already beat IDF against HT40 APs).
