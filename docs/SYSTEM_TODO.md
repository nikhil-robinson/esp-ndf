# Platform work left, by category (ESP32-S3, not peripherals)

Everything that is not a peripheral driver: compute (CPU, vector unit, DSP and AI libraries), memory and data movement,
system control, power, and security. Peripherals are in `docs/TODO.md` §6-7; wireless in `docs/RF_PAUSED.md`;
storage and runtime libraries in `docs/TODO.md` §8-9. Checked against the code on 2026-10-24.

**References** (read before writing ours, per the project rule): IDF v5.3.5 at
`/home/nikhil/idf5_3_3/.espressif/v5.3.5/esp-idf/components` (paths below are relative to it), ESP-DSP at
`bench/idf_dsp/managed_components/espressif__esp-dsp`, ESP-NN from the IDF component manager (`espressif/esp-nn`).

Legend: **done** · **partial** · **none** · TRM = ESP32-S3 TRM v1.8 chapter

## Overview

| Category | Element | Status |
|---|---|---|
| **A. Compute** | A1 CPU cores, exceptions, faults | done |
| | A2 FPU | partial |
| | A3 PIE vector unit (the S3's "AI acceleration", TRM 1) | partial |
| | A4 DSP library (ESP-DSP equivalent) | partial: 8 primitive kernels |
| | A5 AI / NN library (ESP-NN equivalent) | **none** |
| | A6 Math library (libm, fast math) | partial |
| | A7 Multi-core | **done**: xc_call 24x esp_ipc, core stall/unstall |
| | A8 Profiling and debug (perf counters, debug assist, breakpoints, trace) | **none** |
| **B. Memory and data movement** | B1 Cache + MMU, execute from flash (XIP) | **done** (+ fast boot) |
| | B2 Memory map: RTC memory, PSRAM heap, capabilities | **done**: RTC sections, PSRAM heap + caps, 5.5x IDF malloc |
| | B3 PSRAM | done (octal, 8 MB, MMU window) |
| | B4 GDMA | partial |
| | B5 Memory protection (PMS), World Controller | **none** |
| **C. System control** | C1 Clocks (incl. RTC slow clock, 32 kHz crystal) | **done**: slow clock sources + calibration, 32 kHz crystal/osc + its watchdog, RTC fast clock (crystal untested: none fitted) |
| | C2 Reset, restart, reset causes | done |
| | C3 Boot: 2nd-stage loader, app partitions, OTA | **done**: fast-boot loader, ROM-free warm boot 0.79 ms, OTA A/B + rollback (IDF format) |
| | C4 Interrupt matrix | done |
| | C5 System timer, timer groups, software timers | done |
| | C6 Watchdogs (RTC, super, TG1, XTAL32K) | **done**: TG0, TG1, RTC, super (XTAL32K with C1) |
| | C7 Brownout, clock-glitch detection | **done**: brownout (interrupt, IDF levels), glitch + super-WDT resets |
| **D. Power** | D1 Light sleep, deep sleep, wake sources | **none** |
| | D2 Power domains, retention, frequency scaling | **none** |
| | D3 ULP coprocessors (FSM, RISC-V) | **none** |
| **E. Security and crypto** | E1 RNG | **done**: hardware (IDF rate) + ChaCha20 generator, 20-40x IDF |
| | E2 AES accelerator | **done**: ECB/CBC/OFB/CTR/CFB128 + XTS, DMA mode, 1.12-7x IDF; GCM (drivers/crypto/gcm.c) |
| | E3 SHA accelerator | **done**: all modes, CPU + DMA, 1.03-4.6x IDF |
| | E4 RSA / big-number accelerator | **done**: exp/mul mod 1024-4096, first op (R² prep) 1.33-1.56x IDF, exp equal (same engine) |
| | E5 HMAC, Digital Signature (DS) | **none** |
| | E6 Software crypto with no hardware block (ECC, AES-GCM tag, ...) | **done**: P-256 keygen/ECDH/ECDSA 1.55-1.67x, X25519 1.7x, AES-GCM 1.4-2.6x IDF; constant-time |
| | E7 eFuse: fields, key blocks, burn opt-in | **done** (read): IDF field table by name, all 11 blocks, 100/100 vs espefuse; burning not implemented (approval rule) |
| | E8 Flash and PSRAM encryption (XTS_AES), secure boot | **none** (irreversible: plan only) |
| | E9 NVS encryption | done |

## A. Compute

**A1 CPU cores — done.** Dual Xtensa LX7, own vectors and start code, fault handler with registers and exact DWARF
backtrace, interrupt levels 1-5, naked level 4/5 handlers.

**A2 FPU — partial.** Single precision on, own soft-double (`lib/double.*`, faster than IDF ROM). Missing: FPU use
inside interrupt handlers (today a fault; lazy save of the FPU state would allow it).

**A3 PIE vector unit — partial (TRM 1).** The S3 has no separate NPU: AI and DSP acceleration is the PIE 128-bit
vector unit (q0-q7, QACC/ACCX accumulators, `EE.*` instructions, int8/int16 SIMD). Have: enabled at boot, used by our
kernels. Missing: PIE state save so handlers or a second context may use it; QACC (per-lane accumulator) kernels; a
documented instruction cookbook (measured cycle counts, alignment rules, the S3 loop-buffer quirk).

**A4 DSP library — partial.** Have (`dsp.h`, all PIE): `dsp_dot_s16`, `dsp_dot2_s16`, `dsp_add_s16_sat`,
`dsp_dot_f32`, `dsp_copy128(u)`, `dsp_set128`, `dsp_unpack_iq10`; decimating FIR inside `rf_rx_capture_dec`
(12-13x faster than ESP-DSP `dsps_fird_s16`). Missing, each benchmarked against ESP-DSP:
- FFT: radix-2/4, complex and real, int16 and float, bit reversal, windows (Hann, Blackman, flat-top)
- FIR (int16/float, any length), decimating FIR as a general API, IIR biquad (DF1/DF2), cascades
- vector math: mul, scale, MAC, abs, clip, min/max, sum (int8/int16/float)
- matrix: add, mul (int8/int16/float, any shape), transpose
- convolution / correlation, DCT, Kalman filter (ESP-DSP has an EKF for IMU fusion), moving average, peak detection
- Reference: `espressif__esp-dsp/modules/` (fft, fir, iir, dotprod, math, matrix, conv, dct, kalman, windows).

**A5 AI / NN library — none.** An ESP-NN equivalent on PIE, benchmarked layer by layer against ESP-NN on the same
shapes:
- int8 conv2d, depthwise conv2d, fully connected, pooling (max/avg), activation (ReLU, ReLU6, sigmoid, softmax),
  element-wise add/mul, quantise/requantise (per-tensor and per-channel scales, TFLite-compatible rounding)
- memory planning for activations (internal RAM vs PSRAM, DMA prefetch), one small model end to end (e.g. a keyword
  or person-detection net) vs the same model on ESP-NN/TFLite Micro
- Reference: ESP-NN `src/` (convolution, fully_connected, pooling, activation, basic_math).

**A6 Math library — partial.** Have: musl libm (sin, exp, atan2, ...), IEEE-exact float runtime, correctly rounded
strtod/printf. Missing: fast approximate math for control loops (sinf/cosf/atan2f/sqrtf/expf with stated error,
table + polynomial, vector versions on PIE), fixed-point helpers (Q15/Q31).

**A7 Multi-core — done.** xc_call / xc_call_init (cross-core call, examples/xcore_call), xc_stall / xc_unstall (IDF 0x86 sequence); core 1 reset = cpu1_start; per-core interrupt from the other core = xc_call(irq_attach). Was: Have: core 1 start, `xc_seq` / `xc_fifo` / `xc_spin` / `xc_notify`, heap safe from both
cores. Missing: cross-core function call (run fn on the other core, wait for result), core stall/unstall and
single-core reset API, per-core interrupt allocation from the other core. Reference: `esp_system/esp_ipc.c`,
`esp_hw_support/cpu.c`.

**A8 Profiling and debug — none.** Xtensa performance counters (cycles, instruction count, cache misses, stalls) as a
profiling API; ASSIST_DEBUG stack-pointer bounds monitor per core (cheap stack-overflow detection) and memory-region
watch; hardware breakpoints/watchpoints; TRAX trace notes; gdbstub/JTAG guide. Reference: `esp_hw_support/` (debug
assist), `xtensa/` and `esp_cpu` (perfmon, watchpoints).

## B. Memory and data movement

**B1 Cache + MMU, XIP, flash — done (TRM 4, 30).** Instruction + data cache, code and rodata from flash (XIP=lib/all),
fast boot and ROM-free warm boot, flash driver with IDF's chip list, SFDP, QE handling, 4-byte addresses, auto-suspend
(IDF-validated chips), 120 MHz timing tuning. Octal MXIC OPI STR implemented from IDF/ROM (not run: no octal board). Open: octal DTR, cache mapping above 16 MB
(reads there go through SPI1). Reference: `bootloader_support/src/esp32s3/bootloader_esp32s3.c`,
`esp_mm/`, `hal/mmu_hal.c`, `hal/esp32s3/include/hal/cache_ll.h`, `esp_system/ld/esp32s3/sections.ld.in`.

**B2 Memory map — done.** RTC_DATA/BSS/NOINIT sections, PSRAM heap with capabilities (lib/heap.c), examples/mem_test. Was: partial. Have: internal DRAM heap. Missing: RTC fast and RTC slow memory sections (`.rtc.data`,
`.rtc.noinit`; a bench app writes 0x50001000 by hand today), PSRAM heap and capability-based allocation (DMA-capable,
executable, external), reclaiming unused IRAM. Reference: `heap/port/esp32s3/memory_layout.c`,
`esp_system/ld/esp32s3/memory.ld.in`.

**B3 PSRAM — done.** Octal 8 MB, MMU window at 0x3D000000, write-back/invalidate, gapless RF capture into it.

**B4 GDMA — partial (TRM 3).** Have: 5 channels, linked-list descriptors, peripheral connect, memory-to-memory
(sync/async), EOF interrupts. Missing: priority and weight arbitration, burst settings, PSRAM as source/target
(block-size rules, cache handling), descriptor owner checks, error recovery. Reference: `hal/esp32s3/include/hal/gdma_ll.h`,
`esp_hw_support/dma/gdma.c`.

**B5 Memory protection, World Controller — none (TRM 15, 16).** IRAM write-protect, DRAM no-execute, peripheral access
permissions, stack guard regions; secure/non-secure worlds. Reference: `esp_hw_support/port/esp32s3/esp_memprot.c`,
`hal/esp32s3/include/hal/memprot_ll.h`.

## C. System control

**C1 Clocks — done (TRM 7, 17).** soc/esp32s3/rtcclk.c, examples/rtcclk_test; clock output pins not done (no use yet). Was: partial. Have: CPU 240/160/80/40 MHz, APB, peripheral gates/resets. Missing: RTC slow clock
source (RC 136 kHz / 32 kHz crystal / 8MD256) and calibration, RTC fast clock, clock output pins (CLK_OUT1..3),
32 kHz crystal bring-up. Reference: `esp_hw_support/port/esp32s3/rtc_clk.c`, `rtc_time.c`, `hal/esp32s3/include/hal/clk_tree_ll.h`.

**C2 Reset — done.** Reset cause per core with names, `sys_restart`.

**C3 Boot and OTA — done (TRM 8).** soc/esp32s3/loader, drivers/storage/ota.c, examples/ota_test. Was: The ROM loads our image (at 0x0) straight into RAM. Missing: own 2nd-stage loader
(needed for XIP and app selection), app partitions, OTA A/B with rollback, image header + SHA-256 check (needs E3),
fast boot. Reference: `bootloader/subproject/main/`, `bootloader_support/src/bootloader_utility.c`, `app_update/esp_ota_ops.c`.

**C4 Interrupt matrix — done (TRM 9).** Small gaps: NMI (level 7) for a last-resort handler, routing a source to the
other core without running code there.

**C5 Timers — done (TRM 11, 12).** 64-bit time base, delays, software timers, GPTimer.

**C6 Watchdogs — done (TRM 13, 14).** drivers/wdt/wdt.c, examples/wdt_test; XTAL32K watchdog goes with the 32 kHz crystal (C1). Was: Have: TG0 MWDT with a report stage. The ROM's other watchdogs are switched off
at boot. Missing: RTC watchdog (boot-hang protection, chip-reset stage), super watchdog, TG1 MWDT, XTAL32K watchdog.
Reference: `hal/esp32s3/include/hal/rwdt_ll.h`, `mwdt_ll.h`, `esp_system/int_wdt.c`, `esp_system/task_wdt/`.

**C7 Brownout, clock glitch — done (TRM 10, 24).** soc/esp32s3/brownout.c. Was: Reset causes are decoded; the detectors are not configured. Brownout
level + interrupt (cut motors, flush logs before the reset), clock-glitch detector. Reference: `esp_system/port/brownout.c`,
`hal/esp32s3/include/hal/brownout_ll.h`.

## D. Power

**D1 Sleep and wake — none (TRM 10).** Light sleep (RAM kept), deep sleep + wake stub in RTC memory (needs B2), wake
sources (timer, ext0/ext1, GPIO, touch, UART, ULP, USB), RTC time across sleep, measured sleep currents vs IDF.
Reference: `esp_hw_support/sleep_modes.c`, `esp_hw_support/port/esp32s3/rtc_sleep.c`, `hal/esp32s3/include/hal/rtc_cntl_ll.h`.

**D2 Power domains, frequency scaling — none.** Domain control, peripheral retention, sleep pad configuration,
dynamic frequency scaling with locks.

**D3 ULP coprocessors — none (TRM 2).** ULP-FSM and ULP-RISC-V loaders, shared RTC memory, waking the main CPU, RTC I2C
for sensors while asleep. Reference: `ulp/`.

## E. Security and crypto

**E1 RNG — done (TRM 25).** rng_hw / rng_fill / rng_u32 (drivers/crypto/rng.c). Was: True-random API; entropy is full only while RF or the SAR ADC runs (document and handle
both). Reference: `esp_hw_support/hw_random.c`.

**E2 AES — partial (TRM 19).** Have: block mode 128/256 (`aes_block`), XTS-AES-256 in software on top (`aes_xts`),
used by NVS encryption. Missing: DMA mode for large buffers; CBC, CTR, GCM (GHASH in software), CMAC; benchmarks vs
mbedTLS on IDF. Reference: `mbedtls/port/aes/`.

**E3 SHA — done (TRM 18).** drivers/crypto/sha.c. Was: SHA-1/224/256/384/512 (and 512/t), block and DMA mode, HMAC-SHA in software. Needed by C3
(image check). Reference: `mbedtls/port/sha/`, `hal/esp32s3/include/hal/sha_ll.h`.

**E4 RSA / big-number — done (TRM 20).** drivers/crypto/rsa.c (`rsa_mod_init`, `rsa_exp_mod`, `rsa_mul_mod`); vectors from Python pow(). Was: Modular exponentiation and multiplication up to 4096 bits: RSA signatures,
secure boot v2 verification. Reference: `mbedtls/port/bignum/`.

**E5 HMAC, DS — none (TRM 21, 22).** HMAC from eFuse keys (key never readable: NVS key derivation, JTAG re-enable), and
the Digital Signature peripheral (RSA signing with an eFuse-protected key). Reference: `esp_hw_support/esp_hmac.c`,
`esp_ds.c`.

**E6 Software crypto — done.** Have: lib/ecc.c (X25519, RFC 7748) and (P-256 keys, ECDH, ECDSA sign/verify; RFC 6979 vector), drivers/crypto/gcm.c (AES-GCM, 8-bit GHASH). Was: What the S3 has no block for: ECC (P-256 / Curve25519 sign, verify, ECDH), AES-GCM
tag, SHA-3 if needed; constant-time, benchmarked against mbedTLS on IDF.

**E7 eFuse — partial (TRM 5).** Have: raw bits of blocks 1/2, MAC. Missing: full field table (chip revision, flash/PSRAM
config, user blocks, key purposes), virtual eFuses for tests, burning only behind an explicit opt-in.
Reference: `efuse/esp32s3/esp_efuse_table.csv`, `efuse/src/`.

**E8 Flash/PSRAM encryption, secure boot — none (TRM 23).** XTS_AES transparent encryption of flash and PSRAM, secure
boot v2. Both burn eFuses irreversibly: write the plan and get approval before any burn.

**E9 NVS encryption — done.** XTS keys, boot-time reader, host tools.

## Suggested order

1. **B1 XIP + C3 2nd-stage loader** — unblocks RAM for everything else.
2. **E1 RNG + E3 SHA** — small; the boot image check needs them.
3. **C3 OTA A/B with rollback.**
4. **A4 DSP library** (FFT, FIR/IIR, vector, matrix) — **A5 NN library** on top of it.
5. **C6 watchdogs + C7 brownout/glitch** — field robustness.
6. **C1 RTC clocks + B2 RTC memory → D1 sleep/wake → D3 ULP.**
7. **E2 AES modes + DMA, E4 RSA, E5 HMAC/DS, E6 software ECC.**
8. **B4 GDMA advanced, A8 profiling + stack guard, B5 memory protection, A7 cross-core calls, E7 eFuse table.**
9. **E8 flash encryption / secure boot** — plan and approval only.

Each item: read the reference first, own code, IDF/ESP-DSP/ESP-NN/mbedTLS sibling on the same board, numbers in
`PERFORMANCE.md` / `RESULTS.md` in the same commit.
