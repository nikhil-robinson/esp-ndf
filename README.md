# esp-ndf

Bare-metal SDK for the ESP32-S3, built for robotics: STM32-class determinism on
both 240 MHz cores. No RTOS, no ESP-IDF, no ROM calls, no libgcc — every line is ours.

## Measured on hardware (ESP32-S3, 240 MHz)

Interrupt latency, timer match → first instruction of the C handler
(`examples/irq_latency`, 20 000 samples per level):

| Level | esp-ndf | ESP-IDF 5.3 |
|---|---|---|
| 1 | **76 cycles, 0 jitter** | 219–405 cycles |
| 3 | **63 cycles, 0 jitter** | 214–274 cycles |
| 5 (fast path) | **46 cycles (191 ns), 0 jitter** | assembly handlers only |

20 kHz control loop on both cores + 10 kHz timer ISR + USB telemetry
(`examples/bench`, same `bench/bench.h` built for ESP-IDF and esp-hal too):

| | esp-ndf | FreeRTOS (IDF) | esp-hal 1.2 |
|---|---|---|---|
| core1 loop worst case | **0.96 µs** | 10.1 µs | 1.43 µs |
| timer ISR period jitter | **16 ns** | 62 ns | 15 µs (73 ISRs lost) |
| missed 50 µs deadlines | 0 | 1 | 0 |

## Design

- **RAM-only image**: the ROM loader copies it straight into SRAM from flash 0x0.
  No flash cache, so no cache-miss stalls.
- **CALL0 ABI** (no register windows): interrupt entry saves 16 words, never
  spills windows, and window exceptions can't add jitter.
- **Own vectors and dispatcher**: levels 1–3 dispatch in assembly; levels 4–5
  have one direct "fast" handler each. Level 1 drops to INTLEVEL 1 so 2–5 nest
  over it (NVIC-style). The FPU is off inside handlers: float use in an ISR
  faults loudly instead of corrupting state.
- **Faults** print cause, PC and address on the console.

## Layout

```
arch/xtensa/    start.S (both cores), vectors.S (entry/dispatch), irq.c, xtensa.h
soc/esp32s3/    soc.h (TRM register map), soc_intr.h, sys.c (clock/WDT/core1),
                regi2c.c (analog bus), link.ld
drivers/        console/usj.c (USB-Serial-JTAG)
lib/            printf.c, div64.c (replaces libgcc)
include/        public API: ndf.h irq.h sys.h console.h
examples/       bench/, irq_latency/
bench/          bench.h shared benchmark; idf/, idf_irq/ (ESP-IDF), hal/ (esp-hal)
docs/           TRM (not in git)
```

Porting to another chip = a new `soc/<chip>/` (and `arch/riscv/` for RISC-V).

## Build

Needs the Espressif `xtensa-esp32s3-elf` GCC (from ESP-IDF tools) and `esptool.py`.

```
make                              # examples/bench
make APP=examples/irq_latency flash
make monitor
```

`make flash` writes to flash 0x0 (the bootloader slot). Flashing any ESP-IDF
project restores the normal bootloader.
