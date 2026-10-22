# esp-ndf — Pending work

Branch: `ndf-core`. Boards: C9:48 on `/dev/ttyUSB0`, D0 on `/dev/ttyUSB1`.
Host scripts run with `/usr/bin/python3`, `PYTHONPATH=.` from `tools/`.
This is a controlled two-board lab experiment inside a Faraday cage; use plain
engineering wording throughout.

## Approved order

`9.4 → 1 → 3.4 → (4.1) → 2 → 3 → 5 → 8 → 9 → 6 → 4.2 / 4.3 / 4.4 → 7 → 10`

Everything up to and including section 6 (PSRAM capture) is done and committed
(last commit `975bdb3`). The remaining sections are below.

## 4.2 — Drive the Wi-Fi MAC transmit path directly (in progress)

Goal: hand a frame buffer to the Wi-Fi MAC transmit engine from our own driver
(no vendor stack) and confirm the two lab boards see each other on the bench.
This is a loopback link check between our own boards in the cage — a tag/echo
pattern is used only so one board can count the frames from the other.

- Reference register state captured from the vendor PHY's own transmit is in
  `bench/idf_phyble/wifi_tx_state.txt` (MAC window `0x60033000–0x60035000`:
  PLCP registers, descriptor, frame bytes). Diff notes in `wifi_tx_diff.txt`.
- Known MAC registers (from the captured state):
  - PLCP0 `0x60033D08` — descriptor address, low 20 bits; set `0xC0000000` to start.
  - PLCP1 `0x600342F8` — `len[11:0] | rate<<12` (`0x0200B000` for OFDM).
  - HTSIG `0x60034300`, CONF `0x60033D04` — `(conf & 0xffc00fff) | 0x1000`.
  - Trigger sequence: `0x60033C34 |= 2`; `0x60033C40 |= 3`; CONF write;
    `PLCP0 |= 0xC0000000`; poll `0x60033C3C` bit 1.
  - Descriptor word0 = owner(31)|EOF(30)|size<<12|len; word1 = buffer; word2 = next.
  - Captured during a libphy transmit: plcp0 `0x0009bfd0`, plcp1 `0x64`,
    htsig `0x6400`, conf `0x1000`.
### Transmit — DONE

The counter board decodes our frames (len 100, rssi ~−46 dBm, ~195/s limited by
the host serial loop, not the radio). How it works:

- `examples/rf_pkt` gained three commands: `rd addr [n]` (read words),
  `wtx` (build descriptor + 100 B frame, fire via the PLCP0 start bit, poll
  `0x60033c3c` bit 1), and `wup` (tune to 2437 MHz + enable the TX front end via
  the calibrated RF driver, then free the DAC path with `rf_tone_off`).
- `tools/wifi_tx_replay.py`, in one serial session (config is lost if the port is
  reopened — the board resets): `wup`, then replay the **absolute** baseband
  (`0x6001c000`), front-end (`0x60006000`) and MAC (`0x60033000`) register state
  from `bench/idf_phyble/wifi_tx_state2.txt`, then fire in a loop.
- The frame's addr2 is set to the lab source id `02:4e:44:46:00:01` so the counter
  board (`bench/idf_wifisniff`, channel 6) tallies it. Result in
  `bench/rfmeas/wifi_tx_42.txt`.
- Key registers: PLCP0 `0x60033d08` (descriptor low 20 bits | `0xC0000000` start),
  descriptor word0 `0xC0000000|len<<12|len`, word1 buffer, word2 next;
  PLCP1 len `0x600342f8`, HTSIG `0x60034300`, CONF `0x60033d04 = 0x1000`;
  status `0x60033c3c` bit 1 = done.
- Note: both the absolute baseband/FE state AND the calibrated carrier/front-end
  (`wup`) are required — digital config alone transmits nothing.

### 4.2 DONE (end to end)

Receive and the firmware API are complete. `drivers/rf/rf_wifi.c` +
`drivers/rf/rf_wifi_regs.h` (generated register tables), exposed in `include/rf.h`
as `rf_wifi_tx_init` / `rf_wifi_tx` / `rf_wifi_rx_init` / `rf_wifi_rx`, example
`examples/rf_wifi`, host driver `tools/rf_wifi_test.py`, documented in
`docs/RF_API.md`. Firmware-only path (no host replay): 5906/5906 received frames
were ours, len 100, ~984/s (`bench/rfmeas/wifi_rx_42.txt`). RX ring: descriptors
word0/word3 `0x80640000|bufsize` (owner bit 31), MAC clears owner + writes length;
48 B control header, length word at header offset 44.

## 4.3 — 802.15.4  (DONE, transmit only)

`drivers/rf/rf_154.c` (`rf_154_build` in `rf.h`), `examples/rf_154`,
`tools/rf_154_test.py`. Synthesises the O-QPSK/DSSS baseband (2 Mchip/s, 32-chip
PN per 4-bit symbol, half-sine pulse, Q offset half a chip) and plays it at
40 MS/s via `rf_tx_play` on channel 11. Verified on the sibling board: capture
IQ, matched-filter, correlate chips vs the PN table — a run of 8 identical
symbols at full 32/32 correlation = the preamble (`bench/rfmeas/wifi_154_43.txt`).
No 15.4 receiver on these boards, so transmit only.

## 4.4 — Correlator triggers  (DONE)

`rf_corr_trigger` (`drivers/rf/rf_corr.c`, in `rf.h`): slide-correlate a captured
IQ window against a reference waveform; returns the sync alignment (trigger
sample offset) and peak-to-mean ratio. Complements `rf_rx_capture_trigger`
(energy level) with pattern selectivity. `rf_pkt` `corr` command demonstrates it
against a 4-symbol 802.15.4 preamble: signal ratio 30 vs 17.6 silent
(`bench/rfmeas/corr_trigger_44.txt`).

## 7 — Power and front end

- Power-state control and measured current in each state.
- Bluetooth hand-off (sharing the radio with the other core cleanly).
- External antenna / PA switch control.

## 10 — Characterization

Full sweep: frequency accuracy, output level vs index, receive sensitivity,
across the band and over temperature. Roll the numbers into
`docs/PERFORMANCE.md`.

## Open items to note (not blocking)

- Coded-rate receive catch rate varies 0–97 % per run; cause not yet found.
- Whitening-off control not located (left as-is).
- Receive RSSI is offset −22 dB relative to the IDF reading.
- Transmit output drifts about +1 dB over 2 minutes.

## Standing rules (from memory)

- Peripherals first; beat IDF measurably with an IDF sibling for comparison.
- Keep `docs/PERFORMANCE.md` updated with every measured result.
- Keep traces/dumps and their scripts in `bench/*/data` or `bench/rfmeas`, committed.
- USB crash-loop safety: halt on panic, use timeouts, check the port before each run.
- Run the approved list end to end; report at the end, not mid-way.
