# esp-ndf vs ESP-IDF 5.3.5 — measured on hardware

Board: ESP32-S3 devkit (MAC 24:58:7C:E2:C9:D0), CPU 240 MHz on both stacks.
esp-ndf: `make APP=examples/periph_test flash`. ESP-IDF: `bench/idf_periph` (official
driver APIs, IRAM callbacks, `-O2`). Cycles are CPU cycles (4.17 ns). Lower is better.

## Interrupts (`examples/irq_latency` vs `bench/idf_irq`)

| Path, timer match → first C line | esp-ndf | ESP-IDF |
|---|---|---|
| Level 1 | **76 cycles, 0 jitter** | 219–405 |
| Level 3 | **63 cycles, 0 jitter** | 214–274 |
| Level 5 (fast) | **46 cycles, 0 jitter** | asm only |

## GPIO

| | esp-ndf | ESP-IDF |
|---|---|---|
| set + clear | **10 cycles** | 95 |
| pin edge → ISR | **191–192 cycles** | 356–393 |
| dedicated GPIO set + clear | **3 cycles** | 45 |

## GPTimer

| | esp-ndf | ESP-IDF |
|---|---|---|
| read counter | **110 cycles** | 407 |
| alarm → callback | **800 ns, fixed** | 2825–6750 ns |
| 10 kHz period jitter (L1) | **12 cycles** | 828 cycles |
| 10 kHz period jitter (L5 fast path) | **18 cycles** | n/a |
| rate check (1 MHz over 10 ms) | exactly 10000 | — |

## LEDC

| | esp-ndf | ESP-IDF |
|---|---|---|
| duty update | **35 cycles** | 763 |
| reported frequency, 33333 Hz / 8 bit | **33333.333 Hz** (exact, from divider) | 33333 (integer) |

## MCPWM

| | esp-ndf | ESP-IDF |
|---|---|---|
| compare update | **3 cycles** | 75 |
| period interrupt jitter, 20 kHz | **0 cycles** | 21 |
| duty, edge & center aligned (captured) | exact to the 12.5 ns tick | same hardware |
| dead band 1 µs (captured) | 81/81 ticks (±1 capture tick) | same hardware |
| 3-phase sync, 90° | 1001/1000 ticks | same hardware |

## PCNT (x4 quadrature from MCPWM, pads looped back)

| | esp-ndf | ESP-IDF |
|---|---|---|
| 1 MHz, 100 ms, reading continuously (expect 400000) | 400004, 0 backward steps | 400024, 0 backward steps |
| 100 kHz, 1 s, wraps counted by IRQ only | exactly 400000 | — |
| 20 kHz fwd / rev, 10 ms | 800 / −800 | — |
| read count | **85 cycles** | 331 |

Independently checked with the logic analyzer: A and B at 1.000 MHz, B lags 90°.

## RMT (TX GPIO10 → jumper → RX GPIO11, 100 ns ticks)

| | esp-ndf | ESP-IDF |
|---|---|---|
| 100-item frame (> 48: TX refill + RX drain) | 100 received, 0 tick error | 100 received, 0 tick error |
| start transmit (100 items) | **1156 cycles** | 7775 |

## SDM (sigma-delta, 10 MHz, pad sampled 200k times)

| density | esp-ndf | ESP-IDF |
|---|---|---|
| −64 / 0 / +64 (expect 25 / 50 / 75 %) | 25.0 / 50.0 / 75.0 % | 25.0 / 50.0 / 74.9 % |
| density update | **35 cycles** | 318 |

## UART (UART1 TX GPIO12 → jumper → RX GPIO13, 4096 random bytes)

| | esp-ndf | ESP-IDF |
|---|---|---|
| 115200: bytes ok / time (wire 355.6 ms) | 4096, 355.6 ms | 4096, 356.5 ms |
| 2 Mbaud (wire 20.48 ms) | 4096, 20.50 ms | 4096, 20.58 ms |
| 5 Mbaud (wire 8.19 ms) | 4096, 8.21 ms | 4096, 8.27 ms |
| write 16 bytes | **279 cycles** | 2464 |

Baud checked with the logic analyzer (UART2 0x55 on GPIO4): 115197 baud measured for 115200.

## I2C (master I2C0 SDA10/SCL12 ↔ jumpers ↔ slave I2C1 SDA11/SCL13, 100 kHz)

| | esp-ndf | ESP-IDF |
|---|---|---|
| write 16 bytes to slave | 0 wrong, **1551 µs** (≈ wire time) | 0 wrong, 1785 µs |
| write 100 bytes (FIFO chunking) | 0 wrong | 0 wrong |
| read 8 bytes from slave | 0 wrong | — |
| absent address | NACK reported | error reported |
| SCL frequency (analyzer, 400 kHz request) | 399.0 kHz | — |

400 kHz is skipped on this wiring: measured bus rise time with internal
pull-ups is SDA 508 ns / SCL 429 ns, above the 300 ns Fast-mode limit for any
driver. Add 2.2–4.7 kΩ pull-ups to test 400 kHz.

## GDMA (memory to memory, 32 KB)

| | esp-ndf | ESP-IDF (`esp_async_memcpy`) |
|---|---|---|
| 32 KB copy | **63 MB/s**, 0 wrong | 43 MB/s, 0 wrong |

M2M must borrow a valid peripheral id (0–9); an invalid one is refused by the
PMS with a descriptor error (found on hardware).

## SPI (SPI2 master, MOSI GPIO10 → jumper → MISO GPIO11, SCLK GPIO6 on analyzer)

| | esp-ndf | ESP-IDF |
|---|---|---|
| 64 B, 10 / 20 / 40 MHz | 0 wrong, 14649 / 8475 / 5844 cycles | 0 wrong, 17634 / 11469 / 8407 cycles |
| 4 KB DMA @10 MHz (wire 3276 µs) | 0 wrong, 3281 µs | 3338 µs, **RX open item** ¹ |
| 4-byte transaction | **724 cycles** | 4756 |
| SCLK by analyzer (10 MHz) | 10.000 MHz | — |
| master ↔ slave (SPI3), 32 B, modes 0–3 | 0 wrong both directions | — |

¹ In this loopback IDF's 4 KB DMA transfer receives only the first 64 bytes
(unresolved: IDF pin setup for an internal pad loopback; IDF's 64-byte path is correct).
