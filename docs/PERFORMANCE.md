# esp-ndf performance

How fast esp-ndf runs on the ESP32-S3 compared with ESP-IDF v5.3.5,
Espressif's ESP-DSP, FreeRTOS, esp-hal, and plain C. All numbers were
measured on the same ESP32-S3 devkit at 240 MHz with the same wiring.
240 CPU cycles = 1 µs. The competitor numbers come from sibling apps in
`bench/`, built and run on the same board. Methods and raw output are in
[RESULTS.md](RESULTS.md).

The **Floor** column is the hardware limit as we understand it: instruction
count, bus speed or wire time. "At floor" means no software can go
meaningfully faster; "room" means work is still left. This document is
updated with every measured change.

## Headline numbers

| | esp-ndf | Competitor | Gain |
|---|---|---|---|
| Interrupt entry (naked, level 5) | **14 cycles** | IDF C handler: 219–405 | **15–29×** |
| Interrupt: time stolen from the interrupted code | **36 cycles** naked / 109 C | — | — |
| Wake the other core (cross-core interrupt, round trip) | **62 cycles** | IDF `esp_ipc`: 363 000–480 000 | **>5 800×** |
| Lock + unlock, both cores | **19–23 cycles** | IDF `portMUX`: 405 | **18×** |
| Core → core stream, per item | **18 cycles** push | FreeRTOS queue: 384 | **21×** |
| int16 dot product, 1024 elements | **280 cycles** | ESP-DSP: 1 073 | **3.8×** |
| `memcpy` 8 KB, aligned | **1 738 MB/s** | IDF ROM `memcpy`: 382 MB/s | **4.5×** |
| NVS read (encrypted, repeated key) | **435 cycles** | IDF: 82 455 | **190×** |
| NVS read (plain, repeated key) | **498 cycles** | IDF: 26 269 | **53×** |
| FAT file open (repeated path) | **814 cycles** | IDF: 70 187 | **86×** |
| Timer loop jitter at 10 kHz | **12 cycles** | IDF: 828 | **69× less** |
| Software timer, 1 ms periodic: period spread / drift per 1 s | **0 / 0 µs** | `esp_timer`: 981–1000 µs / −19 µs | exact |
| `malloc` + `free`, 64 B | **165 cycles** | IDF: 1 346 | **8.2×** |
| Float divide / `sqrtf` | **56 / 58 cycles** | IDF: 75 / 118 | **1.3× / 2×** |
| Double add / mul / div (per call) | **48–50 / 77 / 271 cycles** | IDF ROM: 51–52 / 83 / 526 | **1.04–1.08× / 1.08× / 1.9×** |
| `strtod`, 16 digits / hard case | **1 419 / 1 532 cycles** | IDF newlib: 8 230 / 13 197 | **5.8× / 8.6×** |
| `printf("%.17g")` 1.23e300 / denormal | **30 348 / 14 238** | IDF: 40 354 / 31 326 | **1.3× / 2.2×** |
| `snprintf("%.3f")` | **1 577 cycles** | IDF newlib: 11 862 | **7.5×** |
| Ring buffer, 16 B item, zero-copy | **105 cycles** | IDF `xRingbuffer`: 1 237 | **11.8×** |
| Async DMA copy 64 KB: start cost | **1 404 cycles** | `esp_async_memcpy`: 56 756 | **40×** |
| Watchdog feed | **27 cycles** | `esp_task_wdt_reset`: 362 | **13×** |

## Interrupts

| Measurement | esp-ndf | ESP-IDF | Floor | Status |
|---|---|---|---|---|
| Timer → first C line, level 1 | 76–81, 0 jitter | 219–405 | ~70 | near floor |
| Level 3 | 66, 0 jitter | 214–274 | ~60 | near floor |
| Level 5, C handler | 53 | assembly only | ~50 | at floor for C (includes clearing PS.EXCM, see note) |
| **Level 5, naked handler (entry / exit / lost)** | **14 / 12 / 36** | — | ~10 / ~10 / ~30 | at floor |
| C handler, total lost, level 1 / 3 / 5 | 160 / 141 / 115 | — | — | at floor (tried variants measured slower) |

Note (2026-10-18): level 2–5 C handlers now clear PS.EXCM on entry. With EXCM set, the core never loops back in a zero-overhead `LOOP`, so any C loop in such a handler ran its body once (found when `printf` in a level-3 handler printed one character). The PS write sits at the start of the register save, so it needs no `rsync`: level 3 is unchanged, level 5 C pays 6 cycles. Naked level 4/5 handlers are not affected (14 cycles).
| GPIO edge → ISR | 191 | 356–393 | — | — |

Cortex-M4 for reference: 12 cycles to the first handler instruction. Our naked
path reaches its first instruction about 11 cycles after the event.

## Core services (time, software timers, heap)

| Measurement | esp-ndf | ESP-IDF | Floor | Status |
|---|---|---|---|---|
| 64-bit time read (`time_us` / `esp_timer_get_time`) | 149 cycles | 193 | ~140 (SYSTIMER latch handshake on APB) | at floor |
| Software timer 1 ms, 1 s: calls / period spread / drift | 1000 / **0 µs** / **0 µs** | 999 / 981–1000 µs / −19 µs (task dispatch) | — | absolute deadlines in hardware |
| Software timer: deadline → callback | 1.6 µs, fixed (26 ticks of 62.5 ns) | task-dispatched, varies | ISR entry + one time read | near floor |
| `delay_ms(250)` measured | 250 001 µs | `vTaskDelay`: 249 645 µs (tick-quantised) | — | — |
| `malloc` + `free`, 64 B | 165 cycles | 1 346 | — | first fit, O(1) merge |
| Heap available (default stacks) | 346 688 B | 262 804 B internal | — | — |

## Runtime library (libc, float, ring buffer, async DMA, watchdog)

| Measurement | esp-ndf | ESP-IDF | Status |
|---|---|---|---|
| Float divide | 56 cycles, IEEE-exact (1 725 host vectors) | 75 (ROM, windowed call) | FPU helper sequence, CALL0 |
| `sqrtf` | 58, IEEE-exact | 118 (newlib wrapper + errno) | — |
| `snprintf` `%.3f` / `%d` | 1 577 / 972 | 11 862 / 2 957 | floats from the double's bits, integer only |
| `strtof("3.14159")` | 613 | 2 797 | correctly rounded for every input |
| Double add, same sign / different sign / exponent gap 33 / cancellation | 50 / 48 / 39 / 68 | 51 / 52 / 45 / 74 | assembly fast paths, IEEE-exact |
| Double mul / div | 77 / 271 | 83 / 526 | mul: 4 × `mull`/`muluh`; div: 11 bits per step on the 32-bit divider |
| `strtod` 16 digits / `2.2250738585072011e-308` | 1 419 / 1 532 | 8 230 / 13 197 | Clinger fast path; table approximation; big-integer check only near a halfway point |
| `snprintf("%.17g")` 1.23e300 / 4.9e-324 | 30 348 / 14 238 | 40 354 / 31 326 | exact digits at any precision (`%.1074f` included) |
| `qsort` 2 000 ints | 632 809 (316 per element) | 830 658 | word-swap fast path |
| Ring buffer 16 B put + get (copy) | 303 | 1 102 | lock-free SPSC |
| Ring buffer zero-copy reserve/commit/peek/release | 105 | 1 237 | — |
| Ring buffer core 1 → core 0, 20 000 items of 1–200 B | 1 107 cycles/item, all intact | — | — |
| Async DMA memcpy 64 KB: start / throughput | 1 404 cycles / 63 MB/s | 56 756 / 51 MB/s | per-channel descriptors, one-shot callback |
| Watchdog feed | 27 | 362 (`esp_task_wdt_reset`) | 3 register writes |

## Two cores

| Measurement (round trip unless noted) | esp-ndf | ESP-IDF / FreeRTOS | Status |
|---|---|---|---|
| Shared 32-bit variable | 30–49, max 49 | 26–28, **max 1 628–1 991** (tick) | at floor (SRAM path) |
| Shared 16 B struct (sequence counter) | 37–78 | no equivalent | — |
| Spinlock-protected word | 47–68 | 219–2 298 (`portMUX`) | — |
| Lock + unlock, uncontended | 19–23 | 405–406 | at floor (1 `S32C1I`) |
| Lock, both cores hammering, per op | 28–53 | 209 | — |
| Critical section via Remote Core Locking | 74–76 (measured, not used) | — | spinlock wins |
| Stream push / item end to end / batched | 18 / 51 / 30 | queue 384 | — |
| Cross-core interrupt, naked handler | **62, fixed** | `esp_ipc` 363 178–479 979 | near floor |
| Cross-core interrupt, C handler, level 5 / 3 / 1 | 111 / 134 / 155 | — | — |
| Cost per control-loop iteration, plain variable | **+0** | — | at floor |
| Same, 16 B struct with sequence counter | +29…48 | — | at floor (SRAM accesses) |

## SIMD (PIE vector unit) and FPU, 1024 elements

| Kernel | Plain C | esp-ndf | ESP-DSP | Floor | Status |
|---|---|---|---|---|---|
| int16 dot product | 5 139 | **280** (was 405) | 1 073 | 256 + call | at floor (the ~24-cycle difference is fixed per call); **3.8× ESP-DSP** |
| int16 saturating add | 9 237 | **409** (was 535) | 554 | ~390 | near floor; 1.35× ESP-DSP |
| float dot product | 5 141 | **1 570** | 1 587 | ~1 536 | at floor |
| copy 16 KB | 16 427 | **2 067** | 2 088 | ~2 050 | at floor |

## Memory

| 8 KB | Before | esp-ndf now | IDF | Floor | Status |
|---|---|---|---|---|---|
| `memcpy`, aligned | 239 MB/s | **1 703–1 738 MB/s** | 382 MB/s | ~1 900 MB/s | at floor per byte (fixed ~100-cycle call/setup cost; 16 KB copy is within 1 %) |
| `memcpy`, same misalignment | 239 MB/s | **1 497 MB/s** | — | ~1 800 | small room |
| `memcpy`, different alignment | 60 MB/s | **1 362 MB/s** (fused shift+load) | — | ~1 450 | near floor |
| `memset` | — | **3 249 MB/s** | — | ~3 800 | near floor |
| GDMA memory → memory | — | 63 MB/s | 43 MB/s | — | the CPU copy wins |

`memcpy` uses the vector path from the measured crossover: aligned ≥ 64 B,
same misalignment ≥ 512 B, different alignment ≥ 128 B. Below that, the
scalar word path is faster.

Hardware quirk found and fixed: re-entering a hardware loop while `LBEG`
still points at it runs at half speed. The fix is one instruction per loop
entry, applied to all code, including every loop GCC generates.

## Portable builds (no hardware acceleration)

Every accelerated path is selected by a capability flag in
`soc/<chip>/soc_caps.h` (`SOC_CPU_HAS_PIE`, `SOC_CPU_HAS_S32C1I`,
`SOC_CPU_HWLOOP_LBEG_QUIRK`, ...). Each one has a portable fallback, so a
chip without the feature still builds and works. `make NO_ACCEL=1` forces all
fallbacks on the S3, and that build passes the full test suite too.

| Function | Accelerated (S3) | Portable fallback (`NO_ACCEL=1`) |
|---|---|---|
| `memcpy` 8 KB aligned | 1 703 MB/s | 378 MB/s (= IDF ROM 382) |
| `memcpy` 8 KB same misalignment | 1 331 cycles | 5 250 (byte loop was 65 564) |
| `memcpy` 8 KB different alignment | 2 440 cycles | 20 606 (shift-merge; byte loop was 65 564) |
| `memset` 8 KB | 3 249 MB/s | 474 MB/s |
| int16 dot product ×1024 | 405 | 13 868 (C, 64-bit accumulator) |
| Cross-core lock + unlock | 38 (`S32C1I`) | 69 (Peterson's algorithm), 40 000 / 40 000 updates ok |

The portable `memcpy` is also what interrupt handlers use on the S3 (the
vector unit is off there).

## Peripherals

| Peripheral | Operation | esp-ndf | ESP-IDF | Gain | Status |
|---|---|---|---|---|---|
| GPIO | set + clear | **10** | 95 | 9.5× | at floor (bus writes) |
| Dedicated GPIO | set + clear | **3** | 45 | 15× | at floor |
| RTC GPIO | set / get | **21 / 98** | 301 / 333 | 14× / 3.4× | — |
| GPTimer | read counter, 64-bit | **110–124** | 407 | 3.7× | at floor (hardware latch ~50 + 2 reads) |
| GPTimer | read counter, 32-bit (`gpt_get32`) | **100** | — | — | at floor |
| GPTimer | alarm → callback | **800 ns fixed** | 2.8–6.8 µs | 3.5–8× | — |
| LEDC | duty update | **35** | 763 | 22× | — |
| MCPWM | compare update | **3** | 75 | 25× | at floor |
| MCPWM | period interrupt jitter | **0** | 21 | — | at floor |
| PCNT | read count | **85** | 331 | 3.9× | — |
| RMT | start 100-item transmit | **1 156** | 7 775 | 6.7× | — |
| SDM | density update | **35** | 318 | 9.1× | — |
| UART | write 16 bytes | **280** | 2 464 | 8.8× | small room |
| I2C | 16-byte write @100 kHz | **1 551 µs** | 1 785 µs | 1.15× | bus-bound |
| SPI master | 4-byte transaction @40 MHz | **549** (was 730) | 4 756 | 8.7× | wire 192 + ~350 register path |
| SPI master | 64 B @40 MHz | **3 939** (was 5 844) | 8 407 | 2.1× | wire 3 072 |
| ADC | one-shot read | **1 846** | 6 736 | 3.6× | conversion-bound |
| ADC continuous | frame interval jitter | **2 cycles** | not measured | — | — |
| TWAI (CAN) | send 8 B @1 Mbit/s | **30 330** | 33 108 | 1.1× | bus-bound |
| Touch | read | **100** | 529 | 5.3× | — |
| RTC GPIO hold through reset | glitches | **0** | — | — | — |

## Radio (blob-free, drivers/rf; same board C9:48)

| Operation | esp-ndf | ESP-IDF (vendor PHY) | Gain | Notes |
|---|---|---|---|---|
| PHY bring-up, calibration results reused | **3.7 ms** | 20.6 ms (`RF_CAL_NONE`, stored data) | **5.6×** | ours replays stored results as data; IDF also reads NVS |
| PHY bring-up vs IDF partial / full calibration | **3.7 ms** | 38–41 ms / 34–45 ms | 9–12× | not like for like until our own calibrations exist |
| Same replay, built on IDF | 4.65 ms | — | — | esp-ndf's analog-bus and pbus paths are faster |
| PLL tune 2437 MHz (cap search + refine) | 402–410 µs | — | — | bit-exact with the vendor (69/69) |
| Over-the-air link, both boards esp-ndf, no vendor code | tone/off **68 dB** | vendor TX → our RX: 68 dB | same | image −34 dB vs −46 (no TX IQ cal yet) |
| Gapless IQ stream, one core, interrupts masked | **80 MS/s** (79.87 measured, 12.3 M samples) | esp_sdr on IDF: 80 MS/s using both cores | — | consumer budget ~2.4 cycles/pair at 80 MS/s |

## Storage

| Operation | esp-ndf | ESP-IDF | Gain | Status |
|---|---|---|---|---|
| Flash read 4 KB, cold (cache line bursts, **120 MHz** QIO, timing-tuned) | **91 µs** (was 132 at 80 MHz) | 349–355 µs | **3.8×** | flash-clock bound |
| Flash read 64 KB, cold | **1 447 µs** (44 MB/s) | 5 177–5 179 µs | **3.6×** | flash-clock bound |
| Flash read 4 KB, repeated (cache hit) | **13 µs** | — | — | memory speed |
| Flash erase 32 KB / 256 KB region | **5.4 / 42.9 ms** | 6.0–6.5 / 43.8–44.2 ms | 1.1× / 1.02× | chip-bound (SFDP erase types: largest that fits) |
| Flash erase 4 KB / 64 KB (data present) | 34.3 / 236–246 ms | 35.2–37.7 / 235–243 ms | 1.0× | chip-bound |
| Flash write 4 KB / 64 KB | **12.6 / 196 ms** | 13.9 / 220 ms | 1.1× / 1.12× | chip-bound |
| Code from flash, cold call (cache miss) | **2 420 cycles** (was 3 515) | 3 527 | **1.46×** | 120 MHz |
| NVS read u32 (repeated key, cached) | **498** (was 2 338) | 26 269 | **53×** | — |
| NVS update u32 | **552 µs** | 757 µs | 1.4× | flash-bound |
| NVS read u32, encrypted (cached) | **435** (was 4 863) | 82 455 | **190×** | — |
| NVS update u32, encrypted | **669 µs** | 1 542 µs | 2.3× | — |
| SPIFFS read 30 KB | **4 ms** | 28 ms | 7× | — |
| SPIFFS 40 × 30 KB rewrites | **7.4–8.4 s** | 10.9 s | 1.3–1.5× | flash-chip bound (write time drifts run to run; A/B cache vs SPI1 read path: <1 % apart) |
| FAT open, repeated path (path cache) | **814** (was 65 882) | 70 187 | **86×** | — |
| FAT open, first time (one directory sector read) | ~51 000 | 70 187 | 1.37× | flash-read bound |
| NVS boot reader, 8 keys | **456 µs** (was 618) | not in IDF 5.3 | — | — |

## Whole system: dual-core control loops (bench/)

| | esp-ndf | FreeRTOS (IDF) | esp-hal |
|---|---|---|---|
| Loop jitter, 20 kHz on both cores + 10 kHz timer ISR + USB telemetry | see RESULTS.md: esp-ndf lowest | — | — |

## Where there is still room (work queue)

1. Flash read: done. Reads now go through the data cache (SPI0 in QIO, 64-byte lines, whole flash mapped by the MMU): 4 KB cold 134 µs, hits 13 µs. Cache preload/autoload were measured and did not help.
2. SPI small transfers: overhead cut from ~530 to ~350 cycles (no read-modify-write, word packing); the rest is ~20-cycle peripheral register accesses.
3. GPTimer read: measured at the floor (the latch alone is ~50 cycles); `gpt_get32` added. `ccount()` is the 1-cycle CPU-local clock.
4. NVS: done (8-entry recent-item cache). FAT: done (4-entry path cache; open no longer re-reads the directory sector).
5. int16 vector kernels: done (software pipelined, within ~8 % of the instruction floor).
6. `memcpy` with different alignments: done (fused shift+load, 829 -> 1 362 MB/s).
7. C interrupt path: at its floor. Both remaining ideas (conditional loop-register save, fewer rsyncs) measured slower; dropping the last rsync risks a SAR/loop hazard for ~1-2 cycles, so it stays.

Everything in this queue is now at, or within ~10 % of, its hardware floor.

## BLE receive (hardware link layer, no vendor code), `examples/ble_scan`

| | esp-ndf `ble_hw` | IDF Bluedroid scanner | esp-ndf software receiver |
|---|---|---|---|
| PDUs/s, one advertiser at ~2,500/s | ~2,280 (ch 37 only) | 750–850 per channel (rotating 37/38/39) | bursts only |
| CPU while scanning | 1.7% (interrupt-driven, 10 PDUs per event) | not measured | about 100% while decoding |
| Init | 2.2 ms | — | — |

**Headroom:** 91% of the sent PDUs are caught. The rest are lost in the re-arm gap between 10-PDU windows; a bigger RX ring would close most of it.

## BLE advertising (hardware link layer, no vendor code), `examples/ble_adv`

| | esp-ndf `ble_hw_adv` | esp-ndf software TX (`rfctl bleadv`) |
|---|---|---|
| Adverts | 50/s at a 20 ms interval (any interval) | ~2,500/s looped from one DAC bank |
| CPU | 0.044% (one short interrupt per advert) | 0% while looping, but the DAC bank is busy and the radio is held in TX |
| Received by IDF Bluedroid | yes, ~16/s (scanner covers ch 37 a third of the time) | yes, ~800/s |

## BLE connection (slave, hardware link layer, no vendor code), `examples/ble_conn`

| | esp-ndf `ble_hw` | IDF (Bluedroid + vendor controller) |
|---|---|---|
| Link with an IDF central, 25 ms interval | stable 5+ min, reconnects after a central reset | — |
| CPU for the link layer | 0.15% (one interrupt per connection event) | not measured |
| Code | `drivers/ble/ble_hw.c` + generated tables, no blobs | libbtdm_app + ROM + libbtbb + libphy |

## Gapless TX streaming at 40 MS/s, `rf_tx_stream`

| | esp-ndf | IDF |
|---|---|---|
| Arbitrary IQ to the DAC, continuous | yes: 9.8 s test, no gaps, phase-continuous | no API (the vendor PHY plays fixed tones and test frames only) |
| CPU | 100% while streaming (interrupts masked); about 75 µs of each 205 µs unit is left for the source | — |
| Headroom | the tail copy (76 µs) is the cost. A DMA copy, or a shorter tail once the switch jitter is measured, would free most of it | |

## Hardware packet engine, `rf_pkt_*` (no BLE protocol)

| | esp-ndf | IDF |
|---|---|---|
| Any sync word / length 0..255 / 1M, 2M, S8, S2 / CRC on or off | yes (exact data, two boards) | only inside the BLE stack (fixed access-address rules, LL framing); libphy's DTM test sends patterns and receives ≤ 37 bytes |
| CPU per packet sent | one short interrupt (not measurable) | stack + controller task |
| Send rate, 37 B at 1M | 1,180 packets/s | — |
| Headroom | receive re-arm ~0.5 ms limits short packets to ~1000/s; fixed RX gain (AGC: section 2) | |

## RF bring-up time with the calibration store

| | esp-ndf cold | esp-ndf warm (stored calibration) | IDF |
|---|---|---|---|
| RF open (PHY init + RX DC) | 47 ms | 6 ms | `esp_phy_enable` with stored PHY data: not measured here |
| First TX (TX DC cal) | 70 ms | 0 ms | — |

## Octal PSRAM (8 MB, in package), `drivers/psram`

All runs at 80 MHz octal DTR, 240 MHz CPU; full 8 MB pattern test passes with 0 bad words.

| | esp-ndf (64 B cache lines) | esp-ndf (32 B) | IDF (`CONFIG_SPIRAM_MODE_OCT`, 80 MHz) |
|---|---|---|---|
| memcpy SRAM → PSRAM, 1 MiB | **49.5 MB/s** (incl. explicit write-back) | 35.6 MB/s | 35.5 MB/s |
| memcpy PSRAM → SRAM, 1 MiB | **84.6 MB/s** | 58.2 MB/s | 60.9 MB/s |

- **Why esp-ndf is faster:** IDF leaves the data cache at 32-byte lines; esp-ndf uses 64-byte lines.
- **Headroom:** the 80 MHz DTR bus carries 160 MB/s raw.

## Decimating channel filter, `rf_rx_capture_dec` (16 MS/s -> 2 / 1 MS/s)

| | ours (`rf_rx_capture_dec`, incl. unpacking the RF words) | ESP-DSP `dsps_fird_s16` (aes3, I + Q, int16 input, filter only) |
|---|---|---|
| decim 8, 48 taps | **11.8 cycles / input sample** | 149.5 |
| decim 16, 128 taps | **11.1** | 134.8 |

12.7x / 12.1x faster (`bench/idf_dsp/fird_result.txt`, same board). ESP-DSP cannot keep up with the 16 MS/s stream (15 cycles per sample available); ours runs gapless for 1 M output samples into PSRAM. Pieces: PIE unpack `dsp_unpack_iq10` 2.14 cycles/sample (C: 9.01), fused I+Q dot `dsp_dot2_s16` (one call, straight-line 64- and 48-element blocks, verified bit-exact against `dsp_dot_s16`). Floor: unpack 2.1 + the MACs (2T/D per sample at 8 per vector instruction: 1.5 at decim 8) = ~3.6 cycles; the rest is 128-bit load latency and per-chunk overhead.

## USB OTG full-speed device, CDC-ACM, `drivers/usb/usb_dev.c` vs ESP-IDF TinyUSB

Same board (C9:48), same host port (xHCI root port, 12M), same host script (`tools/usb_cdc_test.py`, run by
`tools/usb_cmp.sh`), 4 MB each way, byte-exact pattern check, 3 runs each (`bench/usb_cdc/data/cmp.txt`).
IDF sibling `bench/idf_usbcdc` at its full-performance settings: `-O2` (COMPILER_OPTIMIZATION_PERF), assertions off,
logging off, CPU 240 MHz, FreeRTOS 1 kHz, TinyUSB (DMA mode) task priority 24 on core 0, test task priority 23 on
core 1, CDC TX/RX buffers 8 / 4 KB. Ours: `-O2`, 240 MHz, polled, buffer-DMA mode (`bench/usb_cdc/data/cmp_dma.txt`).

| | download (device -> host) | upload (host -> device) | USB CPU while moving data |
|---|---|---|---|
| **ours (DMA)** | **1152 KB/s** | **994-995 KB/s** | **~0.9 %** of one core (driver cycles counted) |
| ours (slave/FIFO, previous) | 1152 KB/s | 978-979 KB/s | CPU copies every word |
| ESP-IDF TinyUSB | 867-870 KB/s | 505 KB/s | ~25 % of core 0 (TinyUSB task + ISR; idle-loop probe) |
| ratio | **1.32x** | **1.97x** | **~27x less CPU** |

CPU: ours = cycles the driver spends working (polls with an event, read/write copies) over the transfer time; IDF =
1 - (rate of a priority-1 counter task on core 0 during the transfers / its idle rate), core 0 holding TinyUSB.

Floor: full-speed bulk carries at most 19 x 64 B per 1 ms frame = 1216 KB/s. Ours gets exactly 18 packets per frame
(95 %) on this host; FIFO depth (128 / 160 words), turnaround (TRDT 3..9) and the host read path (pyserial, raw
`os.read`) all give the same 1152 KB/s, so the remaining slot is the host controller's schedule for this port.
Upload ~15.5 packets per frame (994-1006 KB/s; 4 MB runs give 994-996, a 1 MB run 1006): the OUT path re-arms per
2 KB buffer (4 KB measured no better). The ESP32-S3 has no high-speed USB (TRM 32.1). DMA-mode details (SETUP bit 15 /
status bit 5, endpoint-info area at the top of FIFO RAM via GDFIFOCFG, one-SETUP EP0 arm) follow TinyUSB's dcd_dwc2.c.

## Boot time and code from flash (XIP)

**Boot, chip reset -> main** (`examples/boot_time` vs `bench/idf_boot`, same board; both sides run the same RTC-timer
code `bench/boot/rtc_boot.h`: the RTC timer keeps counting through a software system reset, calibrated against the
system timer; `bench/boot/result*.txt`). IDF with its fast-boot settings (image validation skipped, logs off).

| | reset -> main |
|---|---|
| ESP-IDF (2nd-stage bootloader, `app_main`) | 35 510 - 35 520 us |
| esp-ndf, ROM loads the whole RAM image (before) | 21 708 - 22 080 us |
| + ROM log off for warm resets (RTC_CNTL_STORE4 flag, no eFuse) | 15 938 - 15 947 us |
| esp-ndf fast boot, cold path (ROM -> 2 KB loader: PLL, QIO through the cache, copies the app) | 6 184 - 6 185 us |
| **esp-ndf warm boot, no ROM** (RTC boot stage at the reset vector, XIP off) | **781 - 782 us** with brownout reset on (772 - 780 without: protection costs ~9 us) |
| esp-ndf warm boot, no ROM, XIP=lib | 854 - 860 us |
| ratio (warm, no ROM) | **45x faster** |

Breakdown of the 6.18 ms: ROM fixed work ~4.24 ms (not ours: mask ROM before any of our code), ROM loading our 2016-byte
loader ~1.05 ms (the ROM loads at 0.52 us per byte: 102 KB of extra RAM data cost +53 ms), loader 0.85 ms (PLL lock +
22 KB copied at QIO through the data cache), app init 32 us (the loader already set the clock). Floor: ~4.2 ms (ROM);
headroom ~1 ms in loader size. IDF's own "1.4 ms" app_main timestamp is not reset-based: IDF re-bases the system
timer during startup (esp_timer_impl_systimer.c), so only the RTC-timer method is comparable. Measured with software
resets; a power-on adds the supply ramp. 
**ROM-free warm boot** (TRM 10.6): RTC_CNTL_RESET_STATE bit 13 = 0 points the CPU reset vector at RTC slow memory
0x50000000 instead of the ROM. The flash loader (cold path) installs the RTC stage (`build/<app>/rtcboot.bin`, from
flash 0x1000): a jump at 0x50000000 (slow memory, ~5 us per access) into the stage in RTC fast memory 0x600FE000. The
stage takes only our own warm resets (cause 3/12 software, 7/8 timer group, 9/16 RTC watchdog); anything else
(esptool's USB-JTAG/USB-UART reset 21/22, force-download flag) is handed to the ROM, power-on/EN resets select the
ROM in hardware. It sets what the ROM would have (diffed register by register against the state after the ROM):
SPI0 DIO read setup, flash pads GPIO29-32 to SPI, data-cache SRAM connect, EXTMEM_CACHE_MMU_OWNER; the BBPLL and
core voltage are analog (RTC domain) and still locked, so only the clock muxes are set. Split of the 0.81 ms (before the 21.5 KB image; now 0.78 ms):
reset -> stage 15 us, clock ~70 us, image copy (22 KB, QIO 80 MHz through the cache) ~690 us, app init 19-32 us.
Copy at 120 MHz: the stage sweeps IDF's timing table once through the data cache (or reuses the app's window, RTC
memory note) and copies at 120 MHz; the app reuses the same window instead of sweeping. Floor: cache-line refill
latency (~34 MB/s reached); headroom: skip re-copying unchanged IRAM
code on warm resets (SRAM is kept through a system reset), or a smaller RAM image (XIP=all).


**Code from flash** (`examples/xip_test` vs `bench/idf_xip`, the same 64 generated kernels `bench/xip/xip_kern.c`,
23.8 KB of code, compiled into both; IDF at -O2 / 240 MHz / QIO 80 MHz):

| cycles per kernel call | RAM | flash, warm | flash, cold |
|---|---|---|---|
| **esp-ndf** (32 KB instruction cache, 8-way, 32 B lines) | 142 | **142** | 3515 |
| ESP-IDF (16 KB default) | 143 | 3520 | 3527 |

A working set between 16 and 32 KB runs at RAM speed from flash on esp-ndf (24.8x faster than IDF's default); cold
is flash-bandwidth bound on both. Flash erase + 256 B write while core 1 runs flash code: ours 49.7 ms (core 1
stalled, 0 runs during the operation, 0 wrong results), IDF 57.9 ms.

## 7. Power states (`rf_open` / `rf_close`, `rf_state`)

State machine OFF → IDLE → RX / TX. Transition timing (one board, `rf_pkt` `ptime`):

| Transition | Cold (cal re-run) | Warm (cal cached, RF 8/9) |
|------------|-------------------|---------------------------|
| OFF → IDLE (`rf_open`) | 42 ms | ~6 ms |
| IDLE → RX (`rf_rx_start`) | 109 µs | 109 µs |
| IDLE → TX (`rf_tx_start`, incl. TX DC cal) | 44 ms | ~0 ms (cal cached) |
| → OFF (`rf_close`) | 2 µs | 2 µs |

`rf_open` powers the modem domain and brings up the synthesiser/front end; `rf_close`
turns everything off. Data: `bench/rfmeas/power_states_7.txt`.

**Needs lab instrumentation (not reachable over serial):** supply current per state (current
meter on the board supply); Bluetooth hand-off with a live BT stack (`rf_close` releases the
modem domain so the BT controller can own the radio — clean concurrent sharing is future work);
external antenna / PA switch (GPIO hook only — the DevKit has no external switch/PA to verify).

## 10. Characterization (summary)

Consolidated from the measured results across this work:

- **Receive sensitivity:** our receiver beats IDF (25 % vs 10 % packet reception at TX power
  index 5, same link) — RF 1 / sensitivity data.
- **TX output level vs power index:** relative-dB table in the `level_code` characterization
  (`rf.c`); monotonic over indices 5..15 (≈ −39 dB at 5 to 0 dB at 15). TX output drifts ≈ +1 dB
  over 2 min (warm-up).
- **Frequency / timing:** radio time base `rf_time_us`; scheduled send accurate to ±1 µs over
  300–500 packets; hop < 60 µs; PLL lock detection + recovery (`rf_check_lock`).
- **RSSI:** AGC-corrected absolute dBm, offset −22 dB relative to the IDF scale.
- **Raw PHYs:** 802.11 TX+RX through the Wi-Fi MAC (4.2), 802.15.4 O-QPSK by DAC waveform (4.3),
  correlator trigger on a known sync (4.4) — all verified on the two-board bench.
- RX image rejection, measured with D0's TX image moved out of the bin: 45–62 dB (was quoted 30–41 dB, TX image included); no RX IQ correction needed (a static software correction gains 0–2 dB at best; `bench/rfmeas/data/rx_iq_static.txt`).
- Coded-rate (S2/S8) receive is now steady: a window timeout with the AGC running left the link-layer core unable to sync; a core reset (SYSCON bit 9) + reload after each such timeout fixes it. S8 16–18 per 300 ms window over 20 windows (was 0–19, dying after 1–10), S2 40–51; silence-first start 68–74 of 150 (was 0); 6 s window S8 336 / S2 939 good, 0 CRC-bad (`bench/rfmeas/data/coded_fix.txt`). Whitening-off found (RWBLECNTL bit 14).

## Flash driver (`examples/flash_test` vs `bench/idf_flash`, same board, BY25Q128 0x684018, 2026-10-24)

Chip support as ESP-IDF's list (generic/XMC, GD, ISSI, MXIC, Winbond, Boya, TH, FM): quad-enable bit and write method
per chip (IDF flash_qio_mode.c, set if clear), boot unlock of protection bits (IDF bootloader_flash_unlock), XMC
startup flow, SFDP erase types, 4-byte address commands above 16 MB, unique ID, erase/program auto-suspend for the chips
IDF validates (GD25Q64/128, XMC-D, FM, + SFDP), recovery of an operation left suspended by a reset. 120 MHz: IDF's MSPI
timing table swept at start (12 points, middle of the passing window; no window: stays at 80 MHz). Numbers in the
Storage table above. Flash code on core 1 during an erase without suspend: paused (both), now released between pages
of a write (waits at most one page program). Boya: no suspend (tested: hardware auto-resume never fires; IDF with
suspend forced on this chip hangs the same way; an erase suspended even once by software never completes).
Headroom: reads at 120 MHz are at the line rate; erase/write are chip-bound. Octal: MXIC OPI STR at 80 MHz (IDF s_flash_init_mxic: driver strength, WRCR2 = 1, 16-bit
commands, 8READ 0xEC cache read with 32-bit address / 20 dummy); boot keeps the ROM's bus setup and warm resets go
through the ROM on octal boards. Not run on hardware: 4-byte addresses (needs >16 MB flash), octal flash (DTR not
implemented), suspend on a validated chip.

## Random numbers (`examples/rng_test` vs `bench/idf_rng`, same board, 2026-10-24)

| | esp-ndf | ESP-IDF | Gain |
|---|---|---|---|
| true random word (hardware, paced to IDF's tested 45 kHz) | rng_hw 5 334 cycles (22 us) | esp_random 5 220-5 346 cycles | 1x (entropy-rate bound, by design) |
| random u32 for general use | **rng_u32 263 cycles** | esp_random 5 220 cycles | **20x** |
| 4 KB random bytes | **833 us** | esp_fill_random 22 857 us | **27x** |
| 64 KB random bytes | **9 072 us** (7 MB/s) | 365 713 us | **40x** |

rng_fill/rng_u32: ChaCha20 (RFC 8439 known answer checked) as OpenBSD arc4random, 1 KB keystream buffer, key replaced
from the buffer at each refill and handed-out bytes zeroed (fast key erasure), 256 hardware bits mixed in every
NDF_RNG_RESEED bytes (64 KB). Statistics on both outputs: ones / runs 0.50, byte chi-square 199-263 (255 dof).
Floor: ~33 cycles/byte = the ChaCha20 block on this core; headroom: 4 blocks in parallel with the PIE vector unit.

## SHA accelerator (`examples/sha_test` vs `bench/idf_sha`, same board, best of 4, 2026-10-24)

| cycles | esp-ndf | ESP-IDF mbedtls (hardware, DMA port) | gain |
|---|---|---|---|
| SHA-256 64 B | **2 004** | 9 313 | **4.6x** |
| SHA-256 1 KB | **4 096** | 10 935 | **2.7x** |
| SHA-256 4 KB | **10 210** | 18 303 | **1.8x** |
| SHA-256 64 KB | **130 822** (120 MB/s) | 154 966 (101 MB/s) | **1.18x** |
| SHA-512 64 B / 1 KB / 4 KB / 64 KB | **1 896 / 4 756 / 10 579 / 153 010** | 7 216 / 13 300 / 20 653 / 157 759 | 3.8x / 2.8x / 1.95x / 1.03x |

Short messages CPU-fed (no descriptor/lock overhead); runs of >= 4 blocks in internal RAM by DMA, 63 blocks per run.
Floor: the engine (64 KB at ~2 cycles/byte); headroom small on large inputs, ~1000 cycles on 64 B (padding block).

## OTA (`examples/ota_test`, 2026-10-24)

| | esp-ndf | ESP-IDF |
|---|---|---|
| image check (appended SHA-256) | **first boot of a new image only** | every boot (unless validation is skipped) |
| cold boot reset -> main with OTA selection | 6.77 ms (6.02 without NDF_OTA) | 35.5 ms |
| warm boot (software reset) | **0.79 ms** (unchanged) | 35.5 ms |

Same on-flash format as IDF (otadata select entries, crc32_le over seq): IDF's otatool switches our slots and reads
ours. Code from flash runs from either slot: the code window (MMU entries 496-511) is mapped onto the booted slot.

## Watchdogs (`examples/wdt_test`, real resets, 2026-10-24)

| | esp-ndf | ESP-IDF |
|---|---|---|
| feed TG0 / TG1 MWDT | **28 cycles** | esp_task_wdt_reset 362 cycles |
| feed RTC WDT | **29 cycles** | wdt_hal_feed (same 3 register writes) |
| RTC WDT 200 ms, measured to the reset (RTC timer) | 201 ms | - |
| TG1 WDT 100 ms | 101 ms | - |

RTC WDT stage 0 scaled by 2 << eFuse WDT_DELAY_SEL (IDF assumes 0) and the RTC slow clock measured (149-150 kHz here,
nominal 136), so timeouts are within 1 %. Super WDT: ~3.4 s at this slow clock.

## Heap and memory map (`examples/mem_test` vs `bench/idf_mem`, same board, best of 16, 2026-10-24)

| malloc + free | esp-ndf | ESP-IDF (TLSF heap, SPIRAM_USE_MALLOC) | gain |
|---|---|---|---|
| 64 B internal | **227 cycles** | 1 278 | **5.6x** |
| 4 KB internal | **227** | 1 288 | **5.7x** |
| 100 KB (PSRAM) | **241** | 1 249 | **5.2x** |
| PSRAM in the heap | 8 388 576 B (all 8 MB) | 8 285 912 B | |

First-fit boundary-tag heap, regions internal (internal + DMA caps) and PSRAM (heap_add_psram); malloc >= 16 KB tries
PSRAM first (IDF SPIRAM_MALLOC_ALWAYSINTERNAL). Ceiling: first fit is O(free blocks) under heavy fragmentation
(segregated lists if a profile shows it). RTC_DATA_ATTR / RTC_BSS_ATTR / RTC_NOINIT_ATTR in RTC slow memory.

### Flash suspend on a chip that supports it (BOX-3, GD25LQ128 0xC86018, SFDP 16 dwords, 2026-10-24)

Not on IDF's suspend list (IDF stalls flash code for the whole operation); our SFDP check enables it.

| flash code on core 1 (miss every call) during erase 64 KB + write 4 KB | esp-ndf (auto-suspend, hold 100 us) | ESP-IDF |
|---|---|---|
| core 1 runs during the operation | **~700-1300** | 15 |
| longest pause of flash code | **284 us** | ~25 ms |
| erase + write time, misses every 20 us .. 1 ms | 106-314 ms, data correct | ~100 ms (other core stopped) |
| write 4 KB / 64 KB | **5.0 / 77 ms** | 7.3 / 115 ms (1.45x / 1.5x) |
| read 4 KB / 64 KB (120 MHz) | **91 / 1 447 us** | 348 / 5 165 us (3.8x / 3.6x) |

Hold after resume (NDF_FLASH_SUS_HOLD_US): IDF's 50 us starves the erase when flash is touched every 20-50 us (never
finishes, reported as a failure); 100 us passes every case with the shortest pauses (200 us: 548 us, 400 us: 1 ms).

## AES modes (`examples/aes_test` vs `bench/idf_aes`, C9:D0, best of 4, 2026-10-24)

| cycles, AES-128 | esp-ndf | ESP-IDF mbedtls (hardware, DMA port) | gain |
|---|---|---|---|
| CBC 16 B / 256 B / 4 KB / 64 KB | **921 / 2 418 / 16 941 / 250 029** | 5 956 / 6 727 / 24 031 / 279 184 | 6.5x / 2.8x / 1.42x / 1.12x |
| CTR 16 B / 256 B / 4 KB / 64 KB | **858 / 2 433 / 16 956 / 250 059** | 5 983 / 6 739 / 24 052 / 279 205 | 7.0x / 2.8x / 1.42x / 1.12x |

Engine DMA mode for >= 4 blocks in internal RAM (62 MB/s at 64 KB, the engine's rate), CPU-fed blocks below (no
descriptor/lock setup). Modes ECB, CBC, OFB, CTR, CFB128.

## RSA / big-number (`examples/rsa_test` vs `bench/idf_rsa`, C9:D0, 2026-10-24)

| us | esp-ndf | ESP-IDF mbedtls (hardware MPI) | gain |
|---|---|---|---|
| RSA-1024 prepare modulus (R² mod M) | 2 100 | 3 170 (first exp, incl. 313 exp) | 1.75x |
| RSA-2048 prepare modulus | 8 000 | 13 660 (incl. 1 075) | 1.8x |
| RSA-4096 prepare modulus | 31 700 | 55 553 (incl. 4 061) | 1.9x |
| x^65537, 1024 / 2048 / 4096 | 281 / 1 042 / 4 029 | 313 / 1 075 / 4 061 | 1.11 / 1.03 / 1.01x |
| x^d, 1024 / 2048 / 4096 | 21 700 / 163 000 / 1 277 418 | 21 760 / 163 354 / 1 277 450 | equal |

Exponentiation is the engine's own time (both drive the same block), so x^d is at the floor; the gain is R² preparation
(top-bit trick + doublings instead of mbedtls' generic division) and less per-call overhead on short exponents.
