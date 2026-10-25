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
| 4 | Configurable hardware demodulator | **Partly done** | `gfsk` DAC generator (any rate/deviation, engine frame format) validated: 1M/250 kHz and 2M/500 kHz decode with CRC. Default tolerance at 1M: deviation 200–500 kHz, rate within ±2 % (980/1020 kbit/s: sync, CRC fails). Rate select = modem `0x6001106C` bits 26 (2M) / 27 (coded) + `0x60011070` bit 4; no continuous rate field found. Single-bit sweeps of the modem block and `0x6001CC00` block: nothing makes 100 kHz deviation decode. **Next:** the mode-bit combination test (`bench/rfmeas/demod_modes.py`) was cut off by the D0 USB drop — rerun it; then try multi-bit field values in `0x6001CC00..CCFC` (the words that differ for S2). `data/demod_*.txt` |
| 5 | Receive while transmitting | **Not started** | Plan: a small one-board app with a `selfcap` probe (play a tone, capture at the same time; with the RX front end forced on via `rf_pbus_xpd_rx_on`, and with `rf_rx_on`): find out what the shared address counter captures and whether the RX ADC is gated by TX. |
| 6 | Absolute transmit power | Lab | One calibrated power reading at index 15 / 0 dB; the relative table then gives dBm. |

## Open items carried over

- Receive RSSI offset −22 dB relative to IDF (applied as a constant).
- Transmit output drifts about +1 dB over 2 minutes.
- 1M long packets at 2460/2480 MHz at −71/−74 dBm fail the 5 % CRC limit in `rf_pkt_test` (sensitivity margin; failing in the baseline too).
