# esp-ndf

Bare-metal SDK for the ESP32-S3, built for robotics: STM32-class determinism on
both 240 MHz cores. No RTOS, no ESP-IDF, no ROM calls, no libgcc — every line is ours.

## Measured on hardware (ESP32-S3, 240 MHz)

Interrupt latency, timer match → first instruction of the C handler
(`components/arch/test_apps/irq_latency`, 20 000 samples per level):

| Level | esp-ndf | ESP-IDF 5.3 |
|---|---|---|
| 1 | **76 cycles, 0 jitter** | 219–405 cycles |
| 3 | **63 cycles, 0 jitter** | 214–274 cycles |
| 5 (fast path) | **46 cycles (191 ns), 0 jitter** | assembly handlers only |

20 kHz control loop on both cores + 10 kHz timer ISR + USB telemetry
(`examples/system/bench`, same `bench/bench.h` built for ESP-IDF and esp-hal too):

| | esp-ndf | FreeRTOS (IDF) | esp-hal 1.2 |
|---|---|---|---|
| core1 loop worst case | **0.96 µs** | 10.1 µs | 1.43 µs |
| timer ISR period jitter | **16 ns** | 62 ns | 15 µs (73 ISRs lost) |
| missed 50 µs deadlines | 0 | 1 | 0 |

## Design

- **Interrupt and control code always in RAM**: vectors, handlers, the scheduler and everything an interrupt
  or a flash operation can reach are placed in SRAM. Bulk library code (math, string conversion, storage) runs
  from flash through a 32 KB instruction cache (`XIP=lib`, the default); `XIP=off` keeps the whole image in
  RAM (no cache-miss stalls at all), `XIP=all` also moves the application. Timing numbers in
  [docs/PERFORMANCE.md](docs/PERFORMANCE.md) say which mode they were measured in.
- **CALL0 ABI** (no register windows): interrupt entry saves 16 words, never
  spills windows, and window exceptions can't add jitter.
- **Own vectors and dispatcher**: levels 1–3 dispatch in assembly; levels 4–5
  have one direct "fast" handler each. Level 1 drops to INTLEVEL 1 so 2–5 nest
  over it (NVIC-style). The FPU is off inside handlers: float use in an ISR
  faults loudly instead of corrupting state.
- **Faults** print cause, PC and address on the console.

## Layout (ESP-IDF style)

```
components/
  arch/xtensa/    start.S (both cores), vectors.S (entry/dispatch), irq.c, xtensa.h
  soc/esp32s3/    soc.h (TRM register map), sys.c (clock/WDT/core1), regi2c.c, link.ld.in, loader/
  drivers/<x>/    one folder per peripheral driver (gpio, spi, uart, ... storage, crypto, rf)
  lib/            printf, libm, heap, dsp, nn, ecc, ...
  include/        public API: ndf.h irq.h sys.h console.h ...
  <c>/test_apps/  that component's tests, one application each (docs/TESTING.md)
make/             project.mk (build rules an application includes), boards.mk, unit_main.c
examples/         demonstrations by area: system/ storage/ usb/ bluetooth/ rf/ rf_lab/ box3/
tests/            tests that span components: stress, soak, runtime_test
bench/            ESP-IDF / esp-hal siblings and the data behind docs/PERFORMANCE.md
tools/            host scripts (flashing helpers, ndf_test.py, fuzzers, signing, RF measurement)
docs/             API, configuration, performance, security
```

Porting to another chip = a new `components/soc/<chip>/` (and `components/arch/riscv/` for RISC-V).

## Build

Needs the Espressif `xtensa-esp32s3-elf` GCC (from ESP-IDF tools) and `esptool.py`. An application is a folder with a
three-line Makefile; build it from inside:

```
cd examples/system/bench && make flash monitor
cd components/arch/test_apps/irq_latency && make flash
make test BOARD=devkit               # top level: every unit test on the DEVKIT (docs/TESTING.md)
```

New application: copy any example folder, set `NDF_PATH` to the esp-ndf root, add your sources.

`make flash` writes to flash 0x0 (the bootloader slot). Flashing any ESP-IDF
project restores the normal bootloader.
