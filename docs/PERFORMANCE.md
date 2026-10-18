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
| int16 dot product, 1024 elements | **405 cycles** | ESP-DSP: 1 073 | **2.6×** |
| `memcpy` 8 KB, aligned | **1 738 MB/s** | IDF ROM `memcpy`: 382 MB/s | **4.5×** |
| NVS read (encrypted) | **4 711 cycles** | IDF: 82 455 | **17×** |
| NVS read (plain) | **1 747 cycles** | IDF: 26 269 | **15×** |
| Timer loop jitter at 10 kHz | **12 cycles** | IDF: 828 | **69× less** |

## Interrupts

| Measurement | esp-ndf | ESP-IDF | Floor | Status |
|---|---|---|---|---|
| Timer → first C line, level 1 | 76–81, 0 jitter | 219–405 | ~70 | near floor |
| Level 3 | 63–67, 0 jitter | 214–274 | ~60 | near floor |
| Level 5, C handler | 46–49 | assembly only | ~45 | at floor for C |
| **Level 5, naked handler (entry / exit / lost)** | **14 / 12 / 36** | — | ~10 / ~10 / ~30 | at floor |
| C handler, total lost, level 1 / 3 / 5 | 160 / 138 / 109 | — | ~140 / ~125 / ~100 | ~10 % room |
| GPIO edge → ISR | 191 | 356–393 | — | — |

Cortex-M4 for reference: 12 cycles to the first handler instruction. Our naked
path reaches its first instruction about 11 cycles after the event.

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
| int16 dot product | 5 139 | **405** | 1 073 | ~260 | **room ~1.5×** |
| int16 saturating add | 9 237 | **535** | 554 | ~390 | **room ~1.4×** |
| float dot product | 5 141 | **1 570** | 1 587 | ~1 536 | at floor |
| copy 16 KB | 16 427 | **2 067** | 2 088 | ~2 050 | at floor |

## Memory

| 8 KB | Before | esp-ndf now | IDF | Floor | Status |
|---|---|---|---|---|---|
| `memcpy`, aligned | 239 MB/s | **1 738 MB/s** | 382 MB/s | ~1 900 MB/s | near floor |
| `memcpy`, same misalignment | 239 MB/s | **1 497 MB/s** | — | ~1 800 | small room |
| `memcpy`, different alignment | 60 MB/s | **829 MB/s** | — | ~1 300 | room |
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
| GPTimer | read counter | **110** | 407 | 3.7× | **room** (CPU cycle counter path) |
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

## Storage

| Operation | esp-ndf | ESP-IDF | Gain | Status |
|---|---|---|---|---|
| Flash read 4 KB | **209 µs** (was 269) | 462 µs | 2.2× | room ~1.6× (SPI1 register path; XIP cache next) |
| Flash erase 4 KB | **642 µs** | 1 041 µs | 1.6× | chip-bound |
| Flash write 4 KB | **10.8 ms** | 12.4 ms | 1.15× | chip-bound |
| NVS read u32 | **1 747** (was 2 338) | 26 269 | 15× | room (RAM cache) |
| NVS update u32 | **552 µs** | 757 µs | 1.4× | flash-bound |
| NVS read u32, encrypted | **4 711** | 82 455 | 17× | room |
| NVS update u32, encrypted | **669 µs** | 1 542 µs | 2.3× | — |
| SPIFFS read 30 KB | **4 ms** | 28 ms | 7× | — |
| SPIFFS 40 × 30 KB rewrites | **7.4 s** | 10.9 s | 1.5× | — |
| FAT open (path lookup) | **51 489** (was 65 882) | 70 187 | 1.36× | room (directory cache) |
| NVS boot reader, 8 keys | **456 µs** (was 618) | not in IDF 5.3 | — | — |

## Whole system: dual-core control loops (bench/)

| | esp-ndf | FreeRTOS (IDF) | esp-hal |
|---|---|---|---|
| Loop jitter, 20 kHz on both cores + 10 kHz timer ISR + USB telemetry | see RESULTS.md: esp-ndf lowest | — | — |

## Where there is still room (work queue)

1. Flash read: now 19.6 MB/s (QIO + overlapped buffer halves). The SPI1 register path costs ~47 clocks per 32 B; cache/XIP burst reads are the next step.
2. SPI small transfers: overhead cut from ~530 to ~350 cycles (no read-modify-write, word packing); the rest is ~20-cycle peripheral register accesses.
3. GPTimer read: a read through the timer's latch path; a CPU cycle-counter base would be near-zero cost.
4. NVS and FAT lookups: in-RAM caches of entries and directory blocks.
5. int16 vector kernels: deeper unrolling to hide load latency.
6. `memcpy` with different alignments: a fused shift-and-load variant.
7. C interrupt path: about 10 % left.
