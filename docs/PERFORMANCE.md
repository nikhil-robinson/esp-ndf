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
| Flash read 4 KB, cold (cache line bursts) | **134 µs** (was 269) | 462 µs | **3.4×** | within ~11 % of the per-line floor (~118 µs) |
| Flash read 4 KB, repeated (cache hit) | **13 µs** | — | — | memory speed |
| Flash erase 4 KB | **642 µs** | 1 041 µs | 1.6× | chip-bound |
| Flash write 4 KB | **10.8 ms** | 12.4 ms | 1.15× | chip-bound |
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
