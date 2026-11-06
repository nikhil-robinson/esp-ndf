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

## Re-measurement after the security hardening (2026-10-25)

The security work added stack canaries, an interrupt watchdog, a locked memory-protection split, bounded hardware
waits, index guards on every public driver call, heap block seals, crypto engine locks with key wiping, hedged and
verified ECDSA, constant-time RSA and signed-OTA / loader checks. **Every timing in this document was re-measured on
the final code** (DEVKIT C9:D0, 240 MHz, default configuration: `XIP=lib`, all hardening on); raw console output of
each run is in `bench/remeasure/data/`, the unit tests that print the peripheral numbers are the `*_unit`
applications ([TESTING.md](TESTING.md)). The ESP-IDF peripheral and storage siblings (`bench/idf_periph`, `bench/idf_flash`, `bench/idf_littlefs`) were re-run on this board
the same day (raw output in `bench/remeasure/idf/`): within ~5 % of the IDF figures listed, except flash write / erase, which
are chip-dependent and were replaced by the same-board values. ESP-DSP / esp-hal numbers are unchanged. Sections that were measured on the BOX-3 (DSP, NN, fastmath, ULP, PM) were re-run on the DEVKIT: same chip,
same results within noise.

**What did not move** (within +-3 %): interrupt entry and latency (76 / 66 / 50 cycles, naked level 5: 14 / 11 / 32),
timer jitter, lock / unlock, memcpy / memset, double and float arithmetic, DSP and NN kernels, SHA and AES on long
inputs, flash read / erase / write, XIP cold and warm, I2C / I2S / LCD / ADC / touch / TWAI / UART DMA, ULP, PM, GDMA
copy throughput, PFS and the storage stack. The 60 s DMA + interrupt stress test runs clean (67 005 core-1 rounds, 0 errors); the 24 h soak on the BOX-3 shows 0 failures so far.

**What got slower, and why** (all still ahead of ESP-IDF unless marked):

| Measurement | Before | Now | Cause |
|---|---|---|---|
| `malloc` + `free` 64 B | 165-227 cycles | 284 | heap block seals + free() validation (double free, stray pointer, overflow) |
| AES-128 CBC / CTR, 16 B call | 921 / 858 | 1 413 / 1 337 | engine lock + key-register wipe per call (long inputs: +1 %) |
| `xc_call` round trip | 243 | 262 | bounded waits (+20), cross-core interrupt status check (+5) |
| `gpt_get` 64-bit / 32-bit | 110-124 / 100 | 118-133 / 107 | bounded latch wait |
| `spi_xfer` 4 B, `uart_write` 16 B, `sdm`, RMT start | 549, 280, 35, 1 156 | 592, 301, 37, 1 204 | index guards + bounded waits (+4-8 %) |
| NVS update u32 | 552 us | 628 us | flash program time (chip), same code |
| async DMA 64 KB start | 1 404 cycles | 1 889 | GDMA ownership checks, 17-descriptor chains |
| warm boot, reset -> main (`XIP=lib`) | 0.85-0.88 ms | **1.36 ms** | RAM image 21 -> 34 KB (+0.4 ms copy), init 25 -> 330 us (canary seed, memory protection, watchdog setup) |
| ECDSA sign P-256 | 64 ms | **27 ms** (was 248 ms with verify-after-sign) | new field code + fixed-base table + a guard that recomputes k G instead of verifying; 3.7x faster than IDF with the guard on |
| RSA private x^d 2048 / 4096 | 163 / 1 277 ms | 163 / 1 277 ms (`NDF_RSA_CONST_TIME 1`: 220 / 1 718) | equal to IDF: the same engine, so this is the floor; the constant-time mode is an option, not the default |

Nothing in the document is now slower than ESP-IDF except the rows that were already at parity with ESP-DSP before the
hardening (small matrix multiply, `add_s16`, ~0.95-0.99x: instruction-issue bound, see the DSP table). The RSA private
exponentiation runs on the same hardware block as IDF's and is equal at the floor.

**Found and fixed while measuring** (each also a correctness bug, see security/AUDIT_FINDINGS_firmware.md P7-P12): the
watchdog ISR ran for any interrupt on its shared line (false "not fed" panics); the interrupt watchdog was fed from the
software-timer ISR, which masks all interrupts for ~550 cycles (a 600-cycle level-5 outlier every 250 ms; it now has
its own hardware comparator and costs no latency); the 4 KB GHASH wipe used a byte loop (small GCM 3x slower); the
our libm `sinf` / `cosf` / `atan2f` paid a stack canary (+10 %; those two files are exempt: pure arithmetic);
the memory-protection split could cut the last instruction's prefetch; the first-use RNG seeding (~1 ms) ran at every
boot and now runs on the first random number.

**Not re-run** (hardware not available or untouched by the hardening): everything with two radio boards (BLE, 802.11,
802.15.4, RF bring-up, packet engine, coexistence: the `rf_*` / `ble_*` applications), the USB host throughput
comparison (needs the IDF TinyUSB sibling flashed and a host script), power-state timing, the IDF-side numbers.

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
| NVS read (encrypted, repeated key) | **435 cycles** (not re-run) | IDF: 82 455 | **190×** |
| NVS read (plain, repeated key) | **539 cycles** | IDF: 26 269 | **49×** |
| FAT file open (repeated path) | **840 cycles** | IDF: 70 187 | **84×** |
| Timer loop jitter at 10 kHz | **12 cycles** | IDF: 828 | **69× less** |
| Software timer, 1 ms periodic: period spread / drift per 1 s | **0 / 0 µs** | `esp_timer`: 981–1000 µs / −19 µs | exact |
| `malloc` + `free`, 64 B | **284 cycles** (was 165, see re-measure) | IDF: 1 346 | **4.7×** |
| Float divide / `sqrtf` | **59 / 58 cycles** | IDF: 75 / 118 | **1.3× / 2×** |
| Double add / mul / div (per call) | **48–50 / 77 / 271 cycles** | IDF ROM: 51–52 / 83 / 526 | **1.04–1.08× / 1.08× / 1.9×** |
| `strtod`, 16 digits / hard case | **1 324 / 1 407 cycles** | IDF newlib: 8 230 / 13 197 | **6.2× / 9.4×** |
| `printf("%.17g")` 1.23e300 / denormal | **30 538 / 14 417** | IDF: 40 354 / 31 326 | **1.3× / 2.2×** |
| `snprintf("%.3f")` | **1 191 cycles** | IDF newlib: 11 862 | **10×** |
| Ring buffer, 16 B item, zero-copy | **109 cycles** | IDF `xRingbuffer`: 1 237 | **11.3×** |
| Async DMA copy 64 KB: start cost | **1 889 cycles** | `esp_async_memcpy`: 56 756 | **30×** |
| Watchdog feed | **28 cycles** | `esp_task_wdt_reset`: 362 | **13×** |

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
| `malloc` + `free`, 64 B | 284 cycles (was 165: block-header seals, double-free / overflow detection) | 1 346 | — | first fit, O(1) merge |
| Heap available (default stacks) | 346 688 B | 262 804 B internal | — | — |

## Runtime library (libc, float, ring buffer, async DMA, watchdog)

| Measurement | esp-ndf | ESP-IDF | Status |
|---|---|---|---|
| event post / dispatch to one handler (`tests/runtime_test` vs `bench/idf_runtime`) | **68 / 267 cycles** | 1646 / 1697 (esp_event) | 24x / 6.4x; ISR + other core + overflow tested |
| task switch, cooperative (task -> scheduler -> task, stack guard kept) | **315 cycles** | 8723 (FreeRTOS taskYIELD, measured) | 28x; no preemption by design |
| time() / gmtime_r / mktime / strftime "%F %T" (`components/lib/test_apps/libc_test` vs `bench/idf_time`) | **226 / 767 / 1335 / 1108 cycles** | 3272 / 9662 / 15587 / 70809 (newlib) | 14x / 13x / 12x / 64x; exact vs Python gmtime, years 1..9999 |
| Float divide | 59 cycles, IEEE-exact (1 725 host vectors) | 75 (ROM, windowed call) | FPU helper sequence, CALL0 |
| `sqrtf` | 58, IEEE-exact | 118 (newlib wrapper + errno) | — |
| `snprintf` `%.3f` / `%d` | 1 191 / 1 162 | 11 862 / 2 957 | floats from the double's bits, integer only |
| `strtof("3.14159")` | 561 | 2 797 | correctly rounded for every input |
| Double add, same sign / different sign / exponent gap 33 / cancellation | 50 / 48 / 39 / 68 | 51 / 52 / 45 / 74 | assembly fast paths, IEEE-exact |
| Double mul / div | 77 / 271 | 83 / 526 | mul: 4 × `mull`/`muluh`; div: 11 bits per step on the 32-bit divider |
| `strtod` 16 digits / `2.2250738585072011e-308` | 1 324 / 1 407 | 8 230 / 13 197 | Clinger fast path; table approximation; big-integer check only near a halfway point |
| `snprintf("%.17g")` 1.23e300 / 4.9e-324 | 30 538 / 14 417 | 40 354 / 31 326 | exact digits at any precision (`%.1074f` included) |
| `qsort` 2 000 ints | 632 809 (316 per element) | 830 658 | word-swap fast path |
| Ring buffer 16 B put + get (copy) | 327 | 1 102 | lock-free SPSC |
| Ring buffer zero-copy reserve/commit/peek/release | 109 | 1 237 | — |
| Ring buffer core 1 → core 0, 20 000 items of 1–200 B | 1 107 cycles/item, all intact | — | — |
| Async DMA memcpy 64 KB: start / throughput | 1 889 cycles / 63 MB/s | 56 756 / 51 MB/s | per-channel descriptors, one-shot callback |
| Watchdog feed | 28 | 362 (`esp_task_wdt_reset`) | 3 register writes |

## Two cores

| Measurement (round trip unless noted) | esp-ndf | ESP-IDF / FreeRTOS | Status |
|---|---|---|---|
| Shared 32-bit variable | 30–49, max 49 | 26–28, **max 1 628–1 991** (tick) | at floor (SRAM path) |
| Shared 16 B struct (sequence counter) | 37–78 | no equivalent | — |
| Spinlock-protected word | 47–68 | 219–2 298 (`portMUX`) | — |
| Lock + unlock, uncontended | 19–23 | 405–406 | at floor (1 `S32C1I`) |
| Lock, both cores hammering, per op | 28–55 | 209 | — |
| Critical section via Remote Core Locking | 74–76 (measured, not used) | — | spinlock wins |
| Stream push / item end to end / batched | 18 / 51 / 30 | queue 384 | — |
| Cross-core interrupt, naked handler | **62, fixed** | `esp_ipc` 363 178–479 979 | near floor |
| Cross-core interrupt, C handler, level 5 / 3 / 1 | 148 / 169 / 177 (min; was listed as 111 / 134 / 155, an older measurement) | — | — |
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
`components/soc/<chip>/soc_caps.h` (`SOC_CPU_HAS_PIE`, `SOC_CPU_HAS_S32C1I`,
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
| Temp sensor | read (`components/drivers/tsens/test_apps/tsens_test` vs `bench/idf_tsens`, alternated runs) | **569 cycles** | 1830 cycles | 3.2x | done; values within 0.6 C of IDF (eFuse trim, RC_FAST clock) |
| TWAI | bus-off recovery -> RECOVERED alert, 500k (`test_twai_alerts` vs `bench/idf_twai_ext`) | **2816 us** (floor 2816) | 2831 us | at floor | done; alerts + error states |
| ADC monitor | ADC1 ch 9 > 3000 for 100 ms (`test_adc_filter` vs `bench/idf_adc_ext`) | **every sample** | 0 events (IDF channel encoding bug) | works vs broken | done; IIR filter k=16 matches the model |
| Touch | raw read (`touch_read` vs `touch_pad_read_raw_data`, `bench/idf_touch_ext`) | **99 cycles** | 532 cycles | 5.4x | done; filter/baseline/threshold events/proximity |
| LEDC | duty update (`ledc_duty` vs `ledc_set_duty`+`ledc_update_duty`, `bench/idf_ledc_ext`) | **38 cycles** | 765 cycles | 20x | done; + gamma fade (IDF: none on S3) |
| PCNT | read count (`pcnt_get` 32-bit vs `pcnt_unit_get_count` 16-bit+, `bench/idf_pcnt_ext`) | **91 cycles** | 310 cycles | 3.4x | done; + step notify / index IDF lacks on S3 |
| MCPWM | compare update (`mcpwm_cmp` vs `mcpwm_comparator_set_compare_value`, `bench/idf_mcpwm_ext`) | **3 cycles** | 76 cycles | 25x | done; capture prescale 4 exact (IDF gets 6) |
| RMT WS2812 | 3 LEDs (72 symbols, 10 MHz): call / until done (`test_rmt_ext` vs `bench/idf_rmt_ext`) | **2287 / 23393 cycles** | 3462 / 26956 cycles | 1.51x / 1.15x | done; over the 21600 wire floor 1.8k vs 5.4k |
| I2C 10-bit | write 8 B @100 kHz (`test_i2c_ext` vs `bench/idf_i2c_ext`) | **221.6k cycles** | 230.5k cycles | 1.04x (bus-bound) | done; over the 220.8k wire floor: ~780 vs ~9700 cycles |
| UART RS485 | DE release after the stop bit, 1 Mbaud (`test_uart_modes` vs `bench/idf_uart_modes`) | **669 cycles** | 1311 cycles | 1.96x | done; both from the TX_DONE interrupt (floor: IRQ entry ~100) |
| LCD i80 | 300 B pixel send start, 8-bit (`test_lcd_i80` vs `bench/idf_lcd_i80`) | **1567 cycles** | 4586 cycles | 2.93x | done; both include the 4 us FIFO prefill (960) |
| SPI half duplex | quad write 32 B, cmd+addr+dummy, 10 MHz (`test_spi_hd` vs `bench/idf_spi_hd`) | **2792 cycles** | 5754 cycles | 2.06x | done; floor 2112 (88 SCLK), 790 left |
| I2S TDM | init 4 slots x 16 bit TX (`test_i2s_modes` vs `bench/idf_i2s_modes`) | **24.4k cycles** | 181k cycles | 7.4x | done; stream 0 errors both |
| I2S PDM | init TX+RX duplex (IDF: TX alone) | **27.4k cycles** | 167k cycles | >6.1x | done; loop SNR **99 dB** vs IDF 38 dB (gap was stale ring blocks after restart, fixed) |
| GPIO | set + clear | **10** | 95 | 9.5× | at floor (bus writes) |
| Dedicated GPIO | set + clear | **3** | 45 | 15× | at floor |
| RTC GPIO | set / get | **21 / 98** | 301 / 333 | 14× / 3.4× | — |
| GPTimer | read counter, 64-bit | **118–133** | 407 | 3.1–3.4× | bounded latch wait costs ~10 cycles over the floor |
| GPTimer | read counter, 32-bit (`gpt_get32`) | **107** | — | — | near floor |
| GPTimer | alarm → callback | **875 ns fixed** | 2.8–6.8 µs | 3.2–7.8× | — |
| LEDC | duty update | **35** | 763 | 22× | — |
| MCPWM | compare update | **3** | 75 | 25× | at floor |
| MCPWM | period interrupt jitter | **0** | 21 | — | at floor |
| PCNT | read count | **85** | 331 | 3.9× | — |
| RMT | start 100-item transmit | **1 204** | 7 775 | 6.5× | — |
| SDM | density update | **37** | 318 | 8.6× | — |
| UART | write 16 bytes | **301** | 2 464 | 8.2× | small room |
| I2C | 16-byte write @100 kHz | **1 551 µs** | 1 785 µs | 1.15× | bus-bound |
| SPI master | 4-byte transaction @40 MHz | **592** (was 730) | 4 756 | 8.0× | wire 192 + ~390 register path (index guards, bounded waits) |
| SPI master | 64 B @40 MHz | **4 012** (was 5 844) | 8 407 | 2.1× | wire 3 072 |
| ADC | one-shot read | **1 846** | 6 736 | 3.6× | conversion-bound |
| ADC continuous | frame interval jitter | **2 cycles** | not measured | — | — |
| TWAI (CAN) | send 8 B @1 Mbit/s | **31 038** | 33 108 | 1.07× | bus-bound |
| Touch | read | **100** | 529 | 5.3× | — |
| RTC GPIO hold through reset | glitches | **0** | — | — | — |

## Radio (blob-free, components/drivers/rf; same board C9:48)

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
| Flash erase 32 KB / 256 KB region | **5.07 / 40.4 ms** | 5.64 / 41.6 ms | 1.11× / 1.03× | chip-bound (SFDP erase types: largest that fits) |
| Flash erase 4 KB / 64 KB (blank, this board) | **0.66 / 10.1 ms** | 1.43 / 10.2 ms | 2.2× / 1.01× | chip-bound (a block with data takes the chip 34 / 240 ms on both sides) |
| Flash write 4 KB / 64 KB | **16.0 / 173 ms** | 17.3 / 203 ms | 1.08× / 1.17× | chip-bound; both re-measured on this board (its chip programs slower than the one used on 10-24: 12.6 / 13.9 ms there) |
| Code from flash, cold call (cache miss) | **2 420 cycles** (was 3 515) | 3 527 | **1.46×** | 120 MHz |
| NVS read u32 (repeated key, cached) | **539** (was 2 338) | 26 269 | **49×** | — |
| NVS update u32 | **628 µs** | 757 µs | 1.2× | flash-bound (chip program time varies run to run) |
| NVS read u32, encrypted (cached) | **435** (was 4 863) | 82 455 | **190×** | — |
| NVS update u32, encrypted | **669 µs** | 1 542 µs | 2.3× | — |
| SPIFFS read 30 KB | **4 ms** | 28 ms | 7× | — |
| SPIFFS 40 × 30 KB rewrites | **9.7 s** | 10.9 s | 1.13× | flash-chip bound (write time drifts run to run; A/B cache vs SPI1 read path: <1 % apart) |
| UART DMA (UHCI) 4 KB TX + RX at 5 Mbaud: CPU in driver calls / total | **804 cycles / 8.20 ms** | 36 479 / 8.41 ms | **45×** CPU | wire 8.19 ms (floor) |
| PFS (power-safe FS) vs esp_littlefs 1.14.8: log line 64 B + fsync | **1.28 ms** | 51.3 ms | **40×** | one data program + one 20 B log record; LittleFS rewrites its metadata pair. Floor ~2 page programs (~0.6 ms) |
| PFS read 30 KB | **0.68 ms** | 8.03 ms | **12×** | extent list in RAM: no flash walk |
| PFS remount after workload (~900 log records) | **2.4 ms** | 3.1 ms | 1.3× | CRC + replay; was 17 ms before per-file extent lists, table CRC, O(1) append replay |
| PFS format / first mount | **82 ms / 56 µs** | 84 ms / 453 µs | 1.02× / 8× | log blocks erased lazily (2 erases at format) |
| PFS 40 × 30 KB rewrites / 30 KB write | **16.3 s / 452 ms** | 18.2 s / 466 ms | 1.12× / 1.03× | erase-bound (one 4 KB erase per 4 KB written, both) |
| FAT open, repeated path (path cache) | **840** (was 65 882) | 70 187 | **84×** | — |
| FAT open, first time (one directory sector read) | ~51 000 | 70 187 | 1.37× | flash-read bound |
| NVS boot reader, 8 keys | **201 µs** (was 456) | not in IDF 5.3 | — | — |

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

## BLE receive (hardware link layer, no vendor code), `examples/bluetooth/ble_scan`

| | esp-ndf `ble_hw` | IDF Bluedroid scanner | esp-ndf software receiver |
|---|---|---|---|
| PDUs/s, one advertiser at ~2,500/s | ~2,280 (ch 37 only) | 750–850 per channel (rotating 37/38/39) | bursts only |
| CPU while scanning | 1.7% (interrupt-driven, 10 PDUs per event) | not measured | about 100% while decoding |
| Init | 2.2 ms | — | — |

**Headroom:** 91% of the sent PDUs are caught. The rest are lost in the re-arm gap between 10-PDU windows; a bigger RX ring would close most of it.

## BLE advertising (hardware link layer, no vendor code), `examples/bluetooth/ble_adv`

| | esp-ndf `ble_hw_adv` | esp-ndf software TX (`rfctl bleadv`) |
|---|---|---|
| Adverts | 50/s at a 20 ms interval (any interval) | ~2,500/s looped from one DAC bank |
| CPU | 0.044% (one short interrupt per advert) | 0% while looping, but the DAC bank is busy and the radio is held in TX |
| Received by IDF Bluedroid | yes, ~16/s (scanner covers ch 37 a third of the time) | yes, ~800/s |

## BLE connection (slave, hardware link layer, no vendor code), `examples/bluetooth/ble_conn`

| | esp-ndf `ble_hw` | IDF (Bluedroid + vendor controller) |
|---|---|---|
| Link with an IDF central, 25 ms interval | stable 5+ min, reconnects after a central reset | — |
| CPU for the link layer | 0.15% (one interrupt per connection event) | not measured |
| Code | `components/drivers/ble/ble_hw.c` + generated tables, no blobs | libbtdm_app + ROM + libbtbb + libphy |

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

## Octal PSRAM (8 MB, in package), `components/drivers/psram`

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

## USB OTG full-speed device, CDC-ACM, `components/drivers/usb/usb_dev.c` vs ESP-IDF TinyUSB

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

**Boot, chip reset -> main** (`components/soc/test_apps/boot_time` vs `bench/idf_boot`, same board; both sides run the same RTC-timer
code `bench/boot/rtc_boot.h`: the RTC timer keeps counting through a software system reset, calibrated against the
system timer; `bench/boot/result*.txt`). IDF with its fast-boot settings (image validation skipped, logs off).

| | reset -> main |
|---|---|
| ESP-IDF (2nd-stage bootloader, `app_main`) | 35 510 - 35 520 us |
| esp-ndf, ROM loads the whole RAM image (before) | 21 708 - 22 080 us |
| + ROM log off for warm resets (RTC_CNTL_STORE4 flag, no eFuse) | 15 938 - 15 947 us |
| esp-ndf fast boot, cold path (ROM -> 2 KB loader: PLL, QIO through the cache, copies the app) | 6 184 - 6 185 us |
| **esp-ndf warm boot, no ROM** (RTC boot stage at the reset vector, XIP off) | **781 - 782 us** with brownout reset on (772 - 780 without: protection costs ~9 us); **2026-10-25 with all hardening: see "Re-measurement" at the top** |
| esp-ndf warm boot, no ROM, XIP=lib | 854 - 860 us |
| **same, 2026-10-25 with all hardening** (XIP off / lib / all) | **1.39 / 1.36 / 1.33 ms** (RAM image 37 / 35 / 34 KB, was 21.5 KB; init 276-330 us) |
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


**Code from flash** (`components/soc/test_apps/xip_test` vs `bench/idf_xip`, the same 64 generated kernels `bench/xip/xip_kern.c`,
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

## Flash driver (`components/drivers/flash/test_apps/flash_test` vs `bench/idf_flash`, same board, BY25Q128 0x684018, 2026-10-24)

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

## Random numbers (`components/drivers/crypto/test_apps/rng_test` vs `bench/idf_rng`, same board, 2026-10-24)

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

## SHA accelerator (`components/drivers/crypto/test_apps/sha_test` vs `bench/idf_sha`, same board, best of 4, 2026-10-24)

| cycles | esp-ndf | ESP-IDF mbedtls (hardware, DMA port) | gain |
|---|---|---|---|
| SHA-256 64 B | **2 004** | 9 313 | **4.6x** |
| SHA-256 1 KB | **4 554** | 10 935 | **2.4x** |
| SHA-256 4 KB | **10 728** | 18 303 | **1.7x** |
| SHA-256 64 KB | **132 144** (119 MB/s) | 154 966 (101 MB/s) | **1.17x** |
| SHA-512 64 B / 1 KB / 4 KB / 64 KB | **1 876 / 5 218 / 11 065 / 154 000** | 7 216 / 13 300 / 20 653 / 157 759 | 3.8x / 2.5x / 1.9x / 1.02x |

Short messages CPU-fed (no descriptor/lock overhead); runs of >= 4 blocks in internal RAM by DMA, 63 blocks per run.
Floor: the engine (64 KB at ~2 cycles/byte); headroom small on large inputs, ~1000 cycles on 64 B (padding block).

## OTA (`components/drivers/storage/test_apps/ota_test`, 2026-10-24)

| | esp-ndf | ESP-IDF |
|---|---|---|
| image check (appended SHA-256) | **first boot of a new image only** | every boot (unless validation is skipped) |
| cold boot reset -> main with OTA selection | 6.77 ms (6.02 without NDF_OTA) | 35.5 ms |
| warm boot (software reset) | **0.79 ms** (unchanged) | 35.5 ms |

Same on-flash format as IDF (otadata select entries, crc32_le over seq): IDF's otatool switches our slots and reads
ours. Code from flash runs from either slot: the code window (MMU entries 496-511) is mapped onto the booted slot.

## Watchdogs (`components/drivers/wdt/test_apps/wdt_test`, real resets, 2026-10-24)

| | esp-ndf | ESP-IDF |
|---|---|---|
| feed TG0 / TG1 MWDT | **28 cycles** | esp_task_wdt_reset 362 cycles |
| feed RTC WDT | **29 cycles** | wdt_hal_feed (same 3 register writes) |
| RTC WDT 200 ms, measured to the reset (RTC timer) | 201 ms | - |
| TG1 WDT 100 ms | 101 ms | - |

RTC WDT stage 0 scaled by 2 << eFuse WDT_DELAY_SEL (IDF assumes 0) and the RTC slow clock measured (149-150 kHz here,
nominal 136), so timeouts are within 1 %. Super WDT: ~3.4 s at this slow clock.

## Heap and memory map (`components/soc/test_apps/mem_test` vs `bench/idf_mem`, same board, best of 16, 2026-10-24)

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

## AES modes (`components/drivers/crypto/test_apps/aes_test` vs `bench/idf_aes`, C9:D0, best of 4, 2026-10-24)

| cycles, AES-128 | esp-ndf | ESP-IDF mbedtls (hardware, DMA port) | gain |
|---|---|---|---|
| CBC 16 B / 256 B / 4 KB / 64 KB | **1 413 / 3 096 / 17 664 / 251 301** | 5 956 / 6 727 / 24 031 / 279 184 | 4.2x / 2.2x / 1.36x / 1.11x |
| CTR 16 B / 256 B / 4 KB / 64 KB | **1 337 / 3 016 / 17 584 / 251 206** | 5 983 / 6 739 / 24 052 / 279 205 | 4.5x / 2.2x / 1.36x / 1.11x |

Engine DMA mode for >= 4 blocks in internal RAM (62 MB/s at 64 KB, the engine's rate), CPU-fed blocks below (no
descriptor/lock setup). Modes ECB, CBC, OFB, CTR, CFB128.

## RSA / big-number (`components/drivers/crypto/test_apps/rsa_test` vs `bench/idf_rsa`, C9:D0, 2026-10-24)

| us | esp-ndf | ESP-IDF mbedtls (hardware MPI) | gain |
|---|---|---|---|
| RSA-1024 first x^65537 (prepare R² + exp) | 2 381 | 3 170 | 1.33x |
| RSA-2048 first x^65537 | 9 042 | 13 660 | 1.51x |
| RSA-4096 first x^65537 | 35 681 | 55 553 | 1.56x |
| x^65537, 1024 / 2048 / 4096 | 287 / 1 054 / 4 053 | 313 / 1 075 / 4 061 | 1.09 / 1.02 / 1.00x |
| x^d, 1024 / 2048 / 4096 (engine fast path, default) | 21 733 / 163 333 / 1 277 442 | 21 760 / 163 354 / 1 277 450 | equal (same engine) |
| x^d with `NDF_RSA_CONST_TIME 1` (constant-time engine mode) | 28 807 / 219 867 / 1 717 890 | 21 760 / 163 354 / 1 277 450 | 0.76x / 0.74x / 0.74x |

Exponentiation is the engine's own time (both drive the same block), so x^d is at the floor; the engine's constant-time mode (option) turns the acceleration off and costs 34 %; for a fixed key the fast path shows only the key's length and Hamming weight, the same on every call; the gain is R² preparation
(top-bit trick + doublings instead of mbedtls' generic division) and less per-call overhead on short exponents.

## P-256 ECC (`components/drivers/crypto/test_apps/p256_test` vs `bench/idf_p256`, C9:D0, 2026-10-26)

| us | esp-ndf (components/lib/ecc.c, constant-time) | ESP-IDF mbedtls (default ECP config) | gain |
|---|---|---|---|
| public key (d G) | **13 013** | 92 847 | **7.1x** |
| ECDH shared secret | **39 357** | 92 939 | **2.4x** |
| ECDSA sign (with the fault guard, the default) | **27 136** | 99 645 | **3.7x** |
| ECDSA verify | **54 532** | 197 932 | **3.6x** |

Field: plain residues, an 8x8-limb column (comba) product on `mull`/`muluh` with a dedicated squaring, then the NIST fast
reduction with a fixed number of correction passes (no secret-dependent branch). Points: Jacobian, a = -3. Fixed base
(public key, sign, the u1 G of verify): a 4-bit comb with a 61 KB table of d 16^w G in flash (`p256_comb.c`, generated and
checked by `tools/gen_p256_comb.py`), 64 mixed additions, every entry of a row read. Variable base (ECDH, u2 Q): 4-bit window,
every table entry read. Per primitive (`components/lib/test_apps/ecc_bench`, cycles): field multiply 2 061, square 1 778, add
424, doubling 21.7k, addition 36k; `pmul` 8.7M, `pmul_base` 2.2M.

**Fault guard in sign** (`NDF_ECC_VERIFY_SIGN 1`, default): k G is computed a second time with the scalar n - k (different digits,
different table entries) and compared in Jacobian form, and s k = e + r d is re-checked: a glitch can no longer turn a
signature into a key leak, for the price of one more fixed-base multiplication (+9 ms). `ecc_unit` injects a fault into k G and
into s and checks that no signature leaves. Level 2 additionally verifies the signature (the old guard: 2.5x the sign time).
Headroom: field multiply is 960 product + 1 090 reduction cycles (a Montgomery-friendly reduction for p would save ~400); add / sub
are 3x slower than they need to be (unrolled carry chains).

## AES-GCM (`components/drivers/crypto/test_apps/gcm_test` vs `bench/idf_gcm`, C9:D0, 2026-10-24; key setup included on both sides)

| us | esp-ndf encrypt / decrypt | ESP-IDF mbedtls encrypt / decrypt | gain |
|---|---|---|---|
| 15 B, AAD 20 | 52 / 52 | 120 / 70 | 2.3 / 1.3x |
| 64 B | 58 / 58 | 124 / 75 | 2.1 / 1.3x |
| 100 B, 16-byte IV | 78 / 78 | 152 / 106 | 1.9 / 1.4x |
| 1000 B | 207 / 207 | 387 / 339 | 1.9 / 1.6x |
| 16 KB, AES-128 / 256 | 2 491 / 2 504 | 4 571 / 4 567 (dec 4 530) | 1.8x |

CTR on the engine (DMA) + GHASH with an 8-bit Shoup table in 32-bit words (mbedtls: 4-bit, 64-bit shifts). GHASH is
~2.3 cycles/bit now; headroom: overlap DMA CTR with GHASH of the previous chunk (CTR is ~15% of the 16 KB time).

## X25519 (`components/drivers/crypto/test_apps/x25519_test` vs `bench/idf_x25519`, C9:D0, 2026-10-24)

| us | esp-ndf (components/lib/ecc.c ladder) | ESP-IDF mbedtls Curve25519 | gain |
|---|---|---|---|
| public key (k * 9) | 46 358 | 76 085 | 1.64x |
| shared secret | 45 952 | 77 023 | 1.68x |

Same 8-limb Montgomery field code as P-256 (generic for p = 2^255-19). Headroom: a 2^255-19 specific reduction
(multiply the high half by 38) instead of Montgomery would cut about a third.

## Cross-core call (`components/drivers/xcore/test_apps/xcore_call` vs `bench/idf_xcall`, C9:D0, 2026-10-24)

| cycles, core 0 runs fn on core 1 and waits | esp-ndf `xc_call` | ESP-IDF `esp_ipc_call_blocking` | gain |
|---|---|---|---|
| round trip, min / avg of 10 000 | 262 / 262 | 5 622 / 5 823 | 21.5x |

Ours runs fn in core 1's cross-core interrupt (level 3); IDF wakes an IPC task (context switch each way). Floor: the
bare notify round trip is 127 cycles at level 3 (xcore_notify), so the call adds ~117 cycles of handoff. IDF's
esp_ipc_isr path is assembly-callback only and not compared. Core 1 resumes 261 cycles after xc_unstall.

## Floats in interrupt handlers (`components/arch/test_apps/fpu_isr`, C9:D0, 2026-10-24)

| | esp-ndf | ESP-IDF (ESP32-S3) |
|---|---|---|
| float in a handler | any level 1-5, nested | not allowed (CONFIG_FREERTOS_FPU_IN_ISR: ESP32 only, level 1 only) |
| handler without floats | +0 to +6 cycles round trip (xcore_notify 188/170/141 vs 185/164/143 at L1/L3/L5) | - |
| handler with floats, round trip L3 / L5 | 347 / 320 cycles (int-only handler 147 / 128) | - |

Lazy save: the first FP instruction in a handler traps once, the trap stores f0-f15, FCR, FSR and BR of the
interrupted context in a slot per (core, INTLEVEL); the exit restores them only if CP0 is on (2 instructions when not).
The ~200 extra cycles are the trap (~70), 2 x 19-word save/restore and the handler's own sqrt. IRQ entry latency
(irq_latency): L1 76, L3 66, L5 50 cycles (RESULTS table: 76 / 63 / 46; layout shift, entry code unchanged except L1).

## Profiling and debug (`components/arch/test_apps/debug_test` vs `bench/idf_perf`, C9:D0, 2026-10-24)

| | esp-ndf | ESP-IDF | gain |
|---|---|---|---|
| perf counter read (incl. one CCOUNT read) | 4 cycles (inline RER) | 28 (xtensa_perfmon_value call) | 7x |
| 1000 x 10 nops: cycles / instructions counted | 10 007 / 10 005 (CCOUNT 10 009) | 10 398 / 10 037 (CCOUNT 10 494) | exact vs call overhead |
| stack overflow detection | SP leaves [bottom + 256, top] on any instruction: 0 cycles (fib(24) 1 950 649 both) | FreeRTOS watchpoint on the last 32 bytes (writes only) | - |
| watchpoint hit | callback, access completes, program continues | panic | - |

## GDMA with PSRAM (`components/drivers/gdma/test_apps/gdma_ext` vs `bench/idf_gdma`, C9:D0 octal PSRAM 80 MHz, 2026-10-24)

| MB/s | esp-ndf gdma_memcpy | ESP-IDF esp_async_memcpy | gain | CPU memcpy (ours) |
|---|---|---|---|---|
| internal -> PSRAM, 1 MB | 71 | 67 | 1.06x | 49 |
| PSRAM -> internal, 64 KB | 78 | 72 | 1.08x | 62 |
| PSRAM -> PSRAM, 1 MB | 39 | 37 | 1.05x | 31 |

64-byte external blocks (32-byte: 54 / 58 / 28 MB/s). Both sides are near the PSRAM bus limit (octal DDR 80 MHz =
160 MB/s raw, less command/latency per 64-byte burst); ours saves the per-chunk task/semaphore handoff.
Priority: two channels copying 32 KB at once, the prio-5 channel finishes in ~520 us, the prio-0 one in ~1030 us.

## Memory protection (`components/soc/test_apps/memprot_test`, C9:D0, 2026-10-24)

| | esp-ndf (NDF_MEMPROT=1, default) | ESP-IDF (CONFIG_ESP_SYSTEM_MEMPROT_FEATURE, default) |
|---|---|---|
| code stores (IRAM bus and DRAM alias), data execution | blocked + reported | blocked + reported |
| on a violation | panic with bus/address/type, or a callback (test, logging) | panic |
| run-time cost | 0 (hardware); boot: within the +-40 us run-to-run spread (889 vs 873-918 us) | - |
| lock | optional (NDF_MEMPROT=2) | default on |
| vector patching (irq_naked) | allowed while unlocked | n/a |

Split granularity 256 B: _text_end is aligned to 256 (up to 255 B of padding). Warm boot on C9:D0 is 881-889 us
with or without both features (and at the brownout commit ef16ef2); the 781 us figure was the other devkit.

## PIE in handlers + instruction table (`components/arch/test_apps/pie_isr`, `components/arch/test_apps/pie_bench`, BOX-3, 2026-10-24)

Handler using PIE + FPU, level-3 round trip 198 cycles vs 135 for an integer handler (+63: trap + 208-byte save /
restore). IDF: PIE in ISRs not supported. Full per-instruction table: docs/PIE.md.

## DSP library vs ESP-DSP 1.8.2 (`components/lib/test_apps/dsp_lib` vs `bench/idf_dsplib`, BOX-3, same LCG inputs, best of 3, 2026-10-24)

| cycles | esp-ndf | ESP-DSP 1.8.2 | gain |
|---|---|---|---|
| fft_f32 256 | 12055 | 14332 | 1.19x |
| fft_f32 1024 | 58508 | 68849 | 1.18x |
| fft_f32 4096 | 276695 | 324478 | 1.17x |
| rfft_f32 1024 | 40636 | 58071 | 1.43x |
| fft_s16 1024 | 22310 | 33466 | 1.50x |
| win_hann 1024 | 31414 | 480763 | 15.30x |
| win_blackman 1024 | 42678 | 957984 | 22.45x |
| win_flattop 1024 | 59062 | 1932589 | 32.72x |
| fir_f32 64 taps x 1024 | 118943 | 150103 | 1.26x |
| fird_s16 32 taps /4 x 1024 | 11088 | 19148 | 1.73x |
| biquad_f32 x 1024 | 9284 | 17440 | 1.88x |
| add_f32 1024 | 2102 | 8220 | 3.91x |
| mul_f32 1024 | 2102 | 8220 | 3.91x |
| mulc_f32 1024 | 1848 | 7192 | 3.89x |
| add_s16 1024 | 570 | 557 | 0.98x |
| mul_s16 1024 >>15 | 569 | 684 | 1.20x |
| dot_f32 1024 | 1570 | 1583 | 1.01x |
| mat_mul_f32 16x16x16 | 6647 | 6282 | 0.95x |
| mat_mul_f32 64x64x64 | 361991 | 359021 | 0.99x |
| mat_mul_s16 64x64x64 >>15 | 76177 | 75512 | 0.99x |
| conv_f32 1024 * 64 | 140291 | 276859 | 1.97x |
| corr_f32 1024 . 64 | 110168 | 260962 | 2.37x |
| dct_f32 256 | 19216 | 31129 | 1.62x |

Every function is checked on the chip against a naive double-precision reference (FFTs, FIR, conv/corr, DCT,
matmul, fird) and its probe values match ESP-DSP's (fft_s16 bit-identical; mat_mul_s16 within 1 LSB of ESP's
rounding). Tricks: EE.LDF/STF.128 and .64 feed the FPU four (two) floats per instruction; FPU code scheduled for its
4-cycle latency; radix-4 FFT with a twiddle-free first pass and a cached bit-reversal swap list; windows by float
rotation instead of cosf per sample; FIR / conv / corr share one 4-output kernel with a rotating register window.
At parity, by construction: float and int16 matrix multiply (21 instructions per 16 MACs / 2 per 8 MACs - the same
inner loop as ESP-DSP's aes3 code: the issue floor), add_s16 (3 instructions per 8), dot_f32 (load-bound). ESP-DSP's
add_s16 aes3 ignores its shift argument (output unshifted, unlike its ansi reference); ours honours it.

## int8 NN layers vs ESP-NN 1.4.1 (`components/lib/test_apps/nn_lib` vs `bench/idf_nn`, BOX-3, single core, best of 3, 2026-10-24)

| layer | esp-ndf cycles | ESP-NN 1.4.1 cycles | gain | bit-exact |
|---|---|---|---|---|
| conv1x1 32x32x16->32 | 1007821 | 1138821 | 1.13x | yes |
| conv3x3 16x16x16->32 p1 | 577971 | 627894 | 1.09x | yes |
| dwconv3x3 32x32x32 s1 | 1244039 | 1255255 | 1.01x | yes |
| dwconv3x3 32x32x32 s2 | 313924 | 333808 | 1.06x | yes |
| fc 512->64 | 11815 | 26692 | 2.26x | yes |
| maxpool 2x2 32x32x16 | 16287 | 46268 | 2.84x | yes |
| avgpool 2x2 32x32x16 | 23712 | 120050 | 5.06x | yes |
| add 4096 | 125892 | 152280 | 1.21x | yes |
| relu6 4096 | 1953 | 3909 | 2.00x | yes |
| softmax 100x10 | 120516 | 226604 | 1.88x | yes |
| model end-to-end | 4642787 | 5410648 | 1.17x | yes |

Bit-exact = the weighted output checksum equals ESP-NN's (TFLite int8 arithmetic). Where it comes from: the input
offset folded into a per-channel bias (padded taps fed -in_off so they vanish), PIE int8 dots on im2col rows with a
fused MAC + load and register rotation, the TFLite requantisation as floor((x m + 2^30) / 2^31) + (r + 2^(e-1) -
[r < 0]) >> e (both proven equal to the reference) hand-scheduled in assembly, QACC lane sums for depthwise /
average pooling, int16 PIE arithmetic for 2x2 pooling, 256-entry tables for add and a cached exp table for softmax.
Floor: per-output requantisation (~22 instructions, scalar mull/mulsh as in ESP-NN) bounds conv / dwconv near parity.

## Float math vs IDF newlib (`components/lib/test_apps/fastmath_test` vs `bench/idf_math`, BOX-3, 1024 arguments, 2026-10-24)

| cycles/call | IDF newlib | esp-ndf libm (musl) | esp-ndf fast (scalar / vector) | best gain |
|---|---|---|---|---|
| sinf | 205.5 | 198.3 | 88.2 / 65.0 | 3.2x |
| cosf | 204.7 | 197.6 | 89.8 / 63.4 | 3.2x |
| atan2f | 287.9 | 272.2 | 184.2 | 1.56x |
| expf | 248.7 | 107.9 | 98.5 / 73.0 | 3.4x |
| logf | 229.1 | 126.6 | 128.6 | 1.81x |
| sqrtf | 83.0 | 60.5 (correctly rounded, FPU sqrt0 + Newton) | 68.5 | 1.37x (libm) |

Measured errors (vs double): sin/cos 7.0e-8 abs (|x| <= 8192), atan2 2.4e-7, exp 7.7e-8 rel, log 4.4e-8, rsqrt-based
sqrt 1.6e-7 rel. Vector forms interleave four elements to cover the FPU's 4-cycle latency. Floor: each call rebuilds
its constants (l32r + wfr); a fused vector kernel keeping them in FPU registers would cut ~30% more.

## ULP coprocessors (`components/soc/test_apps/ulp_test` vs `bench/idf_ulp`, BOX-3, 2026-10-24)

| | esp-ndf | ESP-IDF | gain |
|---|---|---|---|
| ULP-RISC-V load (114 B) + start | 273 us | 954 us (ulp_riscv_load_binary + ulp_riscv_run) | 3.5x |
| runs per second at a 10 ms period | 98-99 | 99 | - |

Word-wise RTC slow-memory writes (~us per access) and no full-reserve memset; both sides calibrate the slow clock
beforehand (ours: rtc_slow_hz(), ~2 ms, cached). ULP-FSM: 99 runs/s at 10 ms as well.

## CPU frequency locks (`components/soc/test_apps/pm_test` vs `bench/idf_pm`, BOX-3, 2026-10-24)

| | esp-ndf pm_lock / pm_unlock | ESP-IDF esp_pm CPU_FREQ_MAX lock (DFS 240/80) |
|---|---|---|
| CPU while the task runs without a lock | 80 MHz (measured vs the system timer) | 240 MHz (IDF only scales down from the idle task) |
| lock (80 -> 240, incl. the 40 us voltage settle) | 4886 cycles | 511 cycles (no switch happens) |
| unlock (240 -> 80) | 1662 cycles (no settle needed going down) | 519 cycles (no switch) |

APB stays 80 MHz on both (PLL on), so UART / USB keep running; delay_ms(10) measures 10 002 us at 80 MHz.

## Wi-Fi access point (`components/wifi/test_apps/ap_unit` vs an ESP-IDF softAP, `tools/wl/bench_ap.py`, DEVKIT + C9:48 + BOX-3, 2026-10-29)

Same air, the sniffer (BOX-3) timestamps beacons with the radio's own clock; the station is the IDF peer. Three runs (two open, one WPA2).

| | esp-ndf AP | ESP-IDF softAP | gain |
|---|---|---|---|
| beacon interval (advertised 100 TU = 102 400 us) | 102 397 us mean, min 101 615, max 103 143 | 102 399 us mean, min 99 860, max 104 907 | - |
| beacon jitter (standard deviation, 63 intervals, three runs) | 313-486 us | 512-1103 us | 1.4-2.3x |
| beacon worst case (max - min), runs above | 1.3-2.2 ms | 3.4-5.0 ms | about 2.5x |
| station join + DHCP (IDF `wifi_sta`, 5 joins) | 1.10 s (first 1.8 s) | 1.10 s (first 1.7 s) | equal: the 1.1 s is the peer's own scan and DHCP |

The AP schedules beacons on a fixed 102.4 ms grid from the microsecond timer (a late beacon does not move the next ones); the IDF softAP
runs them from a FreeRTOS timer task. Raw data: `bench/idf_wl_peer/data/ap_compare.txt`.

## Wi-Fi station HT receive, same board (BOX-3; IDF peer image vs esp-ndf `ip_unit`, AP = IDF on C9:48, 2026-10-31)

64 KB TCP echo. Fixes: IDF's 802.11n receive registers (same-board 11b/g/n vs 11b/g diff) and the per-rate TX digital gain
table restored after the receive replay (the replay carried IDF's idle values: the AP heard us far below IDF, and every
higher MCS failed through our lost ACKs, not reception).

| AP | esp-ndf | ESP-IDF |
|---|---|---|
| pinned HT20 MCS0-6 | 94-166 ms | 119-214 ms |
| pinned HT20 MCS7 | stalls | stalls (link limit) |
| unpinned HT20 | 122-151 ms | - |
| unpinned HT40 (IDF AP default) | 117-137 ms (median 121) | 536-808 ms today, 135-225 ms earlier |
| HT40 AP pinned MCS0 / MCS4 | 151-181 / 94-115 ms | 244-397 / 388-538 ms |
| HT40 AP pinned MCS7 | stalls | 1 of 5 hung, 1 of 5 193 s (link limit) |

40 MHz receive (2026-11-01, `NDF_WIFI_HT40`, default on): the station follows the AP's HT Operation element (secondary channel above /
below / none) and we advertise 0x116e as IDF does. Same board, 64 KB echo x5 (`tools/wl/bench_ht_rates.py`, `_idf.py`):

| AP | esp-ndf | 40 MHz frames received | ESP-IDF |
|---|---|---|---|
| ch 9 HT40 below, default rate | 119-160 ms | 339 | 124-136 ms |
| ch 9 HT40 below, MCS7 | 128-138 ms | 327 | 128-313 ms |
| ch 6 HT40 above, default / MCS0 / MCS4 / MCS7 | 117-174 / 148-236 / 126-160 / 145-191 ms | 282-339 | 1298-1642 / 2450-4548 / 3434-3999 / 446-6571 ms (channel 6 busy at that hour) |
| ch 6 HT20, default / MCS7 | 162-234 / 167-255 ms (one 1.4 s) | 0 | 899-1448 ms / - |
| ch 1 HT40, default | 139-178 ms | 327 | 428-815 ms |

How: IDF's station tunes the LO to the 40 MHz centre (regi2c 0x63 words equal ours at channel +-10 MHz) and sets SYSCON 0x6002600c bit 2
(ROM `rom_bb_bss_cbw40_dig`, outside the earlier register captures: the piece that was missing) and 0x6001cc0c bit 0 for secondary below
(`rom_bb_bss_cbw40`), plus FE 0x600060c0, 0x60034268 bit 7, 0x6001ccd0 bit 29 and the HT40 words already in `ht_rx`. The ROM functions are in
`bench/wifi_regs/data/rom_*cbw*.dis`. With 20 MHz only (before), an HT40 AP pinned to an HT40 rate took 8-13 s.
The RSSI the AP reports, same DEVKIT: esp-ndf -20 dBm, IDF -18 dBm (10 samples each, `tools/wl/rssi_same_board.py`); the earlier
-51 vs -26 was the TX gain table above. Data: `bench/idf_wl_peer/data/ht20_mcs_idf_box3.txt`, `ht40_ap_20mhz_station_box3.txt`,
`ht40_receive_box3.txt`, `rssi_same_devkit.txt`.

## Wi-Fi station receive block ack / A-MPDU (BOX-3, AP = IDF on C9:48 with A-MPDU on, 2026-11-02)

The AP's A-MPDU TX was off in the peer image until the station could answer ADDBA (`CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=y` now). The station
answers the AP's ADDBA request (window 16, immediate) and programs one of the MAC's 8 receive block-ack sessions (IDF `hal_agreement_add_rx_ba`,
`bench/wifi_regs/data/idf_hal_rx_ba.dis`); the MAC then acknowledges A-MPDUs and block-ack requests itself. `tools/wl/test_ba.py`: ADDBA 1, 42 frames
received inside A-MPDUs, 256 KB echo intact.

| 64 KB TCP echo, channel 6 busy | median | range |
|---|---|---|
| esp-ndf, block ack on | 502 ms | 400-981 |
| esp-ndf, ADDBA declined | 590 ms | 493-818 (one 1680) |
| ESP-IDF station | 880 ms | 694-1564 |

HT transmit (same day): ping at MCS7 avg 6.0 ms against 13.9 ms at the old 12 Mbit/s default (10 pings, `ip_unit` `rate 0x17`); all HT rates lost every ACK before.

Against IDF about 1.7x; against ourselves without the session the 64 KB gain is inside the noise and the 256 KB echo shows none (1946 vs 2055 ms median):
the echo is limited by our transmit side, not by receiving single frames. Data: `bench/idf_wl_peer/data/ba_rx_compare.txt`.

### Transmit: single frames beat IDF, A-MPDU transmit works but adds nothing (2026-11-02)

TCP upload of 1 MiB to the IDF AP's `iperf_rx` sink, channel 1, BOX-3 (`tools/wl/bench_tcp_up.py`, data `bench/idf_wl_peer/data/tcp_up_compare.txt`):

| station | median kbit/s | range |
|---|---|---|
| esp-ndf, single frames, MCS5 | 5527 | 5213-5596 |
| esp-ndf, A-MPDU transmit, MCS5 | 4800 | 4573-5210 |
| ESP-IDF station (its own rate control) | 3120 | 2922-3478 |

Single-frame transmit is 1.8x the IDF station on the same air. A-MPDU transmit (`NDF_WIFI_AMPDU_TX=1`, 3 MPDUs per aggregate, block ack from the AP) works and is correct (`test_ba_tx.py`) but is 13% slower than our own single frames:
this stack is not air-limited at 5 Mbit/s (the lwIP window and the per-frame path are), and an aggregate waits for a block ack. At MCS7 the bench link is marginal and a third of the aggregates get no block ack (798 against 4217 kbit/s
single frames), so aggregation stays off by default. Where it may pay (a faster sender, UDP floods) was not measured.

## Wi-Fi station join with PMF / WPA3 (DEVKIT esp-ndf `ip_unit` vs IDF 5.3.5 station on the BOX-3, AP = IDF on C9:48, channel 6, 2026-10-30)

Time from the join call to an IP address (both include the 1 s DHCP wait; ours also includes radio init and the scan).

| network | esp-ndf | ESP-IDF station |
|---|---|---|
| WPA2-PSK | 1.78 s | 1.71-1.72 s |
| WPA2 + PMF required (PSK-SHA256) | 1.79 s | 1.72-1.73 s |
| WPA3-SAE | 2.50 s | 2.56-2.58 s |

Equal within the scan time; SAE adds about 0.7 s on both sides (40-round hunting-and-pecking PWE on P-256 in software). Room: the PWE can be
computed during the scan, and our P-256 software multiply is the floor today (no ECC engine use yet).

## DNS and SNTP clients (DEVKIT esp-ndf `ip_unit` vs IDF station on the BOX-3, servers on the IDF AP, `tools/wl/test_dns_sntp.py`, 2026-10-30)

| | esp-ndf | ESP-IDF | gain |
|---|---|---|---|
| DNS lookup (A record, not cached; two runs) | 10.2 / 14.0 ms | 7.2 / 4.7 ms (getaddrinfo) | slower: open item (our NXDOMAIN answer takes 6 ms, ping 4-7 ms; the first lookup likely includes ARP for the server) |
| SNTP: call to the clock set | 4.5-5.9 ms | 1.1-4.7 s (esp_sntp poll mode, first sync) | about 200x: lwIP's SNTP app waits a random startup delay before its first request; ours sends at once |

Data: `bench/idf_wl_peer/data/dns_sntp.txt`. Ours also corrects for half the round trip (4 ms here) where lwIP / IDF set the server's transmit time.

## HTTPS client GET, TLS 1.3 and 1.2 (esp-ndf `https_unit` on DEVKIT vs IDF `esp_http_client` on the BOX-3, both against `esp_https_server` on C9:48, 2026-10-30)

One GET of /hello per connection: TCP connect, full handshake, request, response (no resumption on either side).

| | esp-ndf client | ESP-IDF client | gain |
|---|---|---|---|
| TLS 1.3 | 620-778 ms | about 1250 ms (3 GETs in 3762 ms) | about 1.8x |
| TLS 1.2 | 648-656 ms | about 1230 ms (3698 ms for 3) | about 1.9x |

The IDF server's own handshake work is common to both columns, so the floor is that server; ours uses the board's AES / SHA engines and
our P-256 / X25519 code. Data: `bench/idf_wl_peer/data/tls12_compare.txt`.

## WebSocket echo (esp-ndf `http_unit` on DEVKIT, IDF peer AP with `esp_http_server` :8080/ws, BOX-3 IDF station with `esp_websocket_client`, 2026-10-31)

20 binary messages of 1000 bytes, each sent after the previous echo arrived (round trips), connect = TCP + upgrade.

| | connect | 20 round trips |
|---|---|---|
| esp-ndf client -> IDF server | 17 ms | 439 ms |
| IDF client -> IDF server | 16 ms | 5095 ms |
| IDF client -> esp-ndf server | 24 ms | 624 ms |

2026-11-01, five runs after the TX gain fix: ours -> IDF 457-1639 ms (median 535), IDF -> IDF 4798-5000 ms, IDF -> ours 494-3301 ms
(median 750); the 9-45 s runs of 2026-10-31 were the weak transmit (`bench/idf_wl_peer/data/ws_echo_devkit_after_txgain.txt`).

Relayed through an HT40 AP (ch 6, IDF client on the BOX-3 -> AP -> server on the DEVKIT, 6 sessions, `tools/wl/probe_ws_relay.py`): esp-ndf
server 436-534 ms, IDF `ws_serve` on the same DEVKIT 7362-8128 ms (15x). Before 2026-11-01 ours lost whole messages here with 40 MHz receive on:
the duplicate cache took sequence numbers from every data frame the radio heard, and in 40 MHz mode that includes the AP's frames to the other
station, numbered by the AP per receiver, so retries of ours were dropped as duplicates (a relayed ping: 143 of 200 -> 200 of 200). The station
now passes only its AP's frames to itself or to a group (`components/wifi/sta.c`). With the 3rd board quiet, the AP's 300 pings: 0-2 ms
round trip, 1-38 transmit retries, nothing unacknowledged at HT20 / HT40 above / below (`ping_sta_loss_devkit.txt`, `relay_ping_devkit.txt`,
`ws_relay_ndf_server_devkit_ht40.txt`, `sniff_sta_acks_devkit.txt`).

Ours as client is 11x faster against the same server: `esp_websocket_client` delivers through its own task and polls its socket on a
timeout, while ours runs the echo inside net_poll(). As server, ours answers IDF's client 8x faster than IDF's own server does.
Data: `bench/idf_wl_peer/data/ws_compare.txt`.

## MQTT round trips (esp-ndf `http_unit` on DEVKIT vs esp-mqtt on the BOX-3, test broker on the IDF peer AP, `tools/wl/test_mqtt.py`, 2026-10-31)

20 messages of 100 bytes, each published after the previous one came back through the broker (own subscription).

| | esp-ndf | esp-mqtt | gain |
|---|---|---|---|
| connect (TCP + CONNECT / CONNACK) | 10-14 ms | 12-15 ms | - |
| 20 round trips, QoS 0 | 148 ms | 167 ms | 1.1x |
| 20 round trips, QoS 1 | 4544 ms | 4618 ms | equal |

The QoS 1 time (about 230 ms per message on both) is the test broker's side (lwIP sockets on the peer), not the clients: open item on the
test bench, not a client gap. Data: `bench/idf_wl_peer/data/mqtt_compare.txt`.

## HTTP server (`components/net/test_apps/http_unit` vs ESP-IDF `esp_http_server`, `tools/wl/bench_http.py`, 2026-10-29)

Same IDF client (`esp_http_client`, one connection per request) on the peer's AP "T" (fixed 1 Mbit/s b/g, channel 6); the server is either the esp-ndf board
or the BOX-3 running `esp_http_server`.

| | esp-ndf http server | ESP-IDF esp_http_server |
|---|---|---|
| GET /hello x60 | see `bench/idf_wl_peer/data/http_compare.txt` (about 0.6 req/s) | about 0.7 req/s |
| GET /big (64 KB) x3 | about 1.8 KB/s | about 1.9 KB/s |

Equal within noise: the numbers are limited by the air (1 Mbit/s AP, no carrier sense in our TX path, TCP retransmit timeouts: docs/WIFI_MAC.md "IDF transmit engine"), not by the servers.
No speed-up is claimed here; the server-side cost can only be compared once the TX engine item is closed. Memory: the esp-ndf server needs no heap beyond the lwIP pool (lwIP heap back to baseline after malformed requests, test_http.py).

## TLS 1.3 (`components/tls/test_apps/tls_unit` vs mbedtls in the IDF peer `tls_prims`, ESP32-S3 at 240 MHz, DEVKIT / C9:48, 2026-10-29)

Same chip, same operations: the elliptic-curve and hash/AEAD building blocks of a TLS 1.3 handshake (X25519, ECDSA P-256 / SHA-256, AES-128-GCM).

| operation | esp-ndf | ESP-IDF mbedtls (esp-tls) | gain |
|---|---|---|---|
| X25519 public key | 45.9 ms | 112.2 ms | 2.4x |
| X25519 shared secret | 46.0 ms | 116.8 ms | 2.5x |
| ECDSA P-256 sign | 27.0 ms | 148.2 ms | 5.5x |
| ECDSA P-256 verify | 54.1 ms | 290.7 ms | 5.4x |
| AES-128-GCM, 1 MB in 4096-byte calls | 168.8 ms | 447.1 ms | 2.6x |
| handshake CPU, client (2 verifies + 2 X25519, from these rows) | 203 ms measured | about 810 ms | about 4x |
| handshake CPU, server (sign + 2 X25519) | 121 ms measured | about 377 ms | about 3.1x |

End to end over the air (`tools/wl/test_https.py`, IDF `esp_http_client` / `esp_https_server` as the peer, AP fixed at 1 Mbit/s): IDF client to our server GET /hello 1.28 s including the handshake
(8 requests in 16.6 s), 64 KB through our records in 40 s; our client to the IDF server GET /hello 1.1 s, chunked 64 KB in 4.9 s. Those are air-limited, not CPU-limited. Raw numbers:
`bench/idf_wl_peer/data/tls_prims.txt`. RAM: one connection takes a 7 KB `tls_t`; records are limited to 4096 bytes.


## BLE GATT notification throughput (esp-ndf `ble_gatts` on DEVKIT vs IDF NimBLE server on the BOX-3, IDF NimBLE central on C9:48, 50 ms interval, 1M PHY, `tools/wl/bench_gatt_burst.py`, 2026-11-03)

| case | esp-ndf | ESP-IDF | gain |
|---|---|---|---|
| 244-byte notifications (DLE 251, MTU 247) | 375-382/s = 726-739 kbit/s | 368/s = 719 kbit/s | 1.02-1.04x |
| 4-byte notifications | 1042-1065/s, gaps 0 | 735/s | 1.4x |

Before: 148/s (282 kbit/s). The core ends an event once the exchanges add up to CS+0x20 slots, and we had put the RX ring size (48) there (30 ms); the TX chain was also only
refilled at event end. Now the slot limit follows the interval and the RX interrupt refills the chain. Details in `bench/idf_wl_peer/data/gatts_burst.txt`.


## ESP32-P4 vs ESP-IDF v6.1: interrupts, math / DSP, AI (v1.3 board, esp-ndf 400 MHz vs IDF 360 MHz, same board, 2026-11-06)

IDF cannot run this silicon (rev < 3) above 360 MHz; cycle counts are comparable, wall-clock gains get another 1.11x from the clock. Data: `bench/data/p4_vs_idf.txt`, twins `bench/idf_p4_irq`, `bench/idf_nn`, `bench/idf_dsplib`, `bench/idf_math`.

| case | esp-ndf | ESP-IDF | gain |
|---|---|---|---|
| interrupt latency, level 1/3/5 software interrupt (cycles, min / avg) | 88 / 88 (220 ns) | 242 / 253 (672 ns) | 2.75x |
| same through the interrupt matrix (FROM_CPU) | 108-111 (270 ns) | 242-253 | 2.2x |
| worst case seen (jitter) | 89-118 | 1672-4060 | 14-35x |
| sinf / cosf / atan2f / expf / logf (cycles) | 88 / 81 / 127 / 91 / 100 | 182 / 181 / 223 / 150 / 170 | 1.7-2.2x |
| fft_f32 1024 / 4096 | 80,387 / 375,681 | 98,512 / 475,277 (fft4r) | 1.23-1.27x |
| rfft_f32 1024 | 47,019 | 76,481 | 1.63x |
| window hann / blackman / flat-top 1024 | 20.6k / 29.9k / 48.3k | 521k / 1.04M / 2.09M | 25-43x |
| fft_s16 1024 | 24,775 | 34,075 | 1.38x (was 177,870 on the C path; the last two stages now run on PIE: VUNZIP / VZIP / 64-bit half loads) |
| NN conv3x3 16x16x16->32 | 843,814 | 898,578 | 1.06x |
| NN depthwise 3x3 32x32x32, stride 1 / 2 | 1,398,348 / 351,158 | 1,890,226 / 472,980 | 1.35x |
| NN max / avg pool 2x2 32x32x16 | 10,853 / 63,905 | 21,608 / 102,503 | 2.0x / 1.6x |
| NN add 4096 / softmax 100x10 | 160,188 / 86,895 | 216,716 / 157,531 | 1.35x / 1.8x |
| NN fully connected 512->64 / relu6 4096 | 12,669 / 4,180 | 14,908 / 4,160 | 1.18x / 0.99x (loads fused into the multiply-accumulates; relu6 unrolled by four) |
| NN model end to end | 6,496,586 (16.2 ms) | 7,226,888 (20.1 ms) | 1.11x cycles, 1.24x time |

All NN outputs are bit-identical to esp-nn. The PIE kernels need the v2.1 instruction encodings on v1.x silicon (`-march=..._xespv2p1`, soc.mk).

### P4 memcpy / memset (`components/lib/test_apps/mem_p4` vs `bench/idf_mem`, same board, cycles, warm, 2026-11-06)

| case | esp-ndf | ESP-IDF | gain |
|---|---|---|---|
| memcpy 8 KB aligned | 1,646 (2.0 GB/s) | 5,289 | 3.2x |
| memcpy 1 KB aligned | 302 | 708 | 2.3x |
| memcpy 256 B | 171 | 199 | 1.16x |
| memcpy 8 KB mixed alignment | 3,910 | 9,173 | 2.3x |
| memset 8 KB / 1 KB | 1,162 / 236 | 3,112 / 423 | 2.7x / 1.8x |
| memcpy 16 / 64 B | 76 / 121 | 52 / 95 | 0.7-0.8x (IDF faster; the timer reads are a large part of these) |
| memmove overlapping (either way) | same as IDF | | 1.0x |

The copy / set kernels are the PIE ones (128-bit), used outside interrupt handlers only: `mintstatus.mil` is zero, the unit is on, no watchpoint handler runs on top of the code (`ndf_vec_block`), internal RAM. Interrupt handlers and early boot take the scalar path (tested). memcmp and strlen go a word at a time on the P4.

### P4 crypto engines (`components/drivers/crypto/test_apps/{aes,sha,gcm}_test` vs `bench/idf_p4_crypto`, same board, 2026-11-06)

| case | esp-ndf | ESP-IDF v6.1 | gain |
|---|---|---|---|
| AES-128-CBC / CTR 16 B (cycles) | 1,624 | 3,518 | 2.2x |
| AES-128-CBC 256 B / 4 KB / 64 KB | 7,194 / 21,774 / 250,813 | 9,465 / 42,904 / 258,136 | 1.3x / 2.0x / 1.03x |
| SHA-256 64 B / 1 KB / 4 KB / 64 KB | 2,671 / 6,681 / 14,391 / 158,321 | 9,295 / 14,474 / 23,079 / 176,465 | 3.5x / 2.2x / 1.6x / 1.1x |
| SHA-512 64 B / 1 KB / 4 KB / 64 KB | 2,512 / 7,121 / 11,801 / 108,416 | 5,784 / 15,108 / 21,839 / 138,527 | 2.3x / 2.1x / 1.9x / 1.3x |
| AES-128-GCM 64 B / 1000 B / 16 KB (us) | 29 / 47 / 193 | 110 / 145 / 303 | 3.8x / 3.1x / 1.6x |
| P-256 pubkey / sign (us) | 5,622 / 11,386 | 5,017 / 13,063 | 0.9x / 1.15x |
| P-256 ECDH (us) | 4,951 (was 17,923 in software) | 5,252 (p256-m, software) | 1.06x |
| P-256 verify (us) | 10,294 (was 24,199) | 23,968 | 2.3x |

Before this work the P4 engines ran 3-30x slower than these (AES-CBC 64 KB 7.6 M cycles, SHA-256 64 KB 1.95 M, GCM 16 KB 6.2 ms). Causes: the crypto
clock (`clk_sec`, PERI_CLK_CTRL25 bits 13:12) was left on XTAL while IDF's `esp_crypto_clk_init` selects the 240 MHz PLL (the engine registers read 61 instead
of 26 cycles and the engine ran 3.3x slower: it speeds up every engine); AES block modes and SHA ran register-fed per block instead of by AXI-GDMA; GCM did
GHASH in software, it now runs the engine's GCM mode (H, CTR, GHASH, tag in hardware, 96-bit IV; other IV lengths keep the software path). The ECC engine
was on the same slow clock (point multiplication 29.5 ms, now 4.9 ms): P-256 ECDH and signature verification now run on the ECC / ECDSA engines.

### P4 libc / lib functions (`components/lib/test_apps/{lib_p4,mem_p4}` vs `bench/idf_p4_lib`, `bench/idf_mem`, same board, cycles, best of 8, 2026-11-06)

| case | esp-ndf | ESP-IDF v6.1 | gain |
|---|---|---|---|
| snprintf "%d" | 781 | 1,382 | 1.8x |
| snprintf "%.3f" | 935 | 4,949 | 5.3x |
| snprintf "%s %5d %x" | 1,091 | 1,661 | 1.5x |
| strtod "3.14159265" | 911 | 1,265 | 1.4x |
| qsort 1000 ints | 214,727 | 254,001 | 1.18x |
| malloc + free 64 B / 4 KB | 325 / 324 | 1,742 / 1,740 | 5.4x |
| crc32 4 KB | 28,211 (was 45,084) | 38,947 (ROM table) | 1.4x |
| md5 4 KB | 60,725 (was 152,679) | 58,779 | 0.97x (parity) |
| strtol "123456" | 202 (was 483) | 169 | 0.84x (IDF faster) |
| flash read 4 KB / 64 KB (us) | 490 / 7,864 (was 1,739 / 27,827) | 516 / 7,816 | 1.05x / 0.99x |
| flash erase 4 KB / 64 KB (us) | 12,022 / 22,750 | 12,999 / 30,408 | 1.08x / 1.34x |
| flash write 4 KB / 64 KB (us) | 5,833 / 91,179 | 8,645 / 135,809 | 1.5x |
| PSRAM 200 MHz write / read (MB/s) | 92 / 120 | 82 / 108 | 1.12x / 1.11x |

The crc32 slice-by-8, the unrolled md5 and the strtol fast path are RISC-V-only code (the S3 build is byte-identical). Flash reads go through a 64 KB MMU window
and the L2 cache like code fetches (`cache_flash_read`), instead of one 64-byte SPI1 command per read.
