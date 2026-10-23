# System elements: status and what is left (ESP32-S3)

System-level parts of the chip, not peripherals: CPU, memory, cache/MMU, DMA, interrupts, clocks, reset, boot,
timers, watchdogs, power, multi-core, security. Checked against the code on 2026-10-24 (not against `docs/TODO.md`,
which is partly out of date). TRM = ESP32-S3 TRM v1.8 chapter. "Reference" = where IDF (v5.3.5, local copy
`/home/nikhil/idf5_3_3/.espressif/v5.3.5/esp-idf/components`) already solves it: read it before writing ours.

Legend: **done** · **partial** · **none**

## Summary

| # | Element | TRM | Status | Main gap |
|---|---|---|---|---|
| 1 | Cache + MMU, execute from flash (XIP) | 4 | partial | data cache only (flash reads, PSRAM); code must fit in internal RAM |
| 2 | Memory map, RTC memory, heap regions | 4 | partial | no RTC fast/slow sections, no PSRAM heap |
| 3 | GDMA | 3 | partial | no priorities/weights, no PSRAM (external memory) DMA |
| 4 | Interrupt matrix | 9 | done | NMI (level 7) unused; cross-core routing only from the target core |
| 5 | Reset and clock | 7, 17 | partial | RTC slow/fast clock source + calibration, CLK_OUT, frequency scaling |
| 6 | Chip boot / bootloader / OTA | 8 | none | ROM loads the app directly; no 2nd stage, no A/B, no image check |
| 7 | System timer, timer groups | 11, 12 | done | — |
| 8 | Watchdogs | 13, 14 | partial | only TG0 MWDT; RWDT, super WDT, TG1, XTAL32K WDT not used |
| 9 | Brownout, clock-glitch detection | 10, 24 | none | reset causes decoded, detectors not configured |
| 10 | Low-power: light/deep sleep, wake, power domains | 10 | none | no sleep at all |
| 11 | ULP coprocessors (FSM, RISC-V) | 2 | none | — |
| 12 | Multi-core | 17 | partial | no cross-core function call, no per-core stall/reset API |
| 13 | Memory protection (PMS), World Controller | 15, 16 | none | no IRAM write-protect, no stack guards |
| 14 | Debug assist, perf counters, trace | 4/17, Xtensa | none | no stack-bound monitor, no perfmon, no breakpoints |
| 15 | CPU extensions: FPU, PIE | 1 | partial | not allowed in ISRs (no lazy save) |
| 16 | eFuse | 5 | partial | read MAC / raw bits only |
| 17 | RNG | 25 | none | — |
| 18 | SHA | 18 | none | needed for OTA image checks |
| 19 | AES | 19 | done | DMA mode (CBC/CTR/GCM, large buffers) missing |
| 20 | RSA, HMAC, Digital Signature | 20-22 | none | — |
| 21 | Flash / PSRAM encryption (XTS_AES), secure boot | 23 | none | irreversible eFuse: plan only until approved |

## Details

### 1. Cache + MMU, execute from flash (XIP) — partial (TRM 4)
- Have: data cache over SPI0 in QIO for flash reads (`flash.c`, NDF_FLASH_READ_AUTO); MMU entries for the PSRAM window
  at 0x3D000000 (`psram.c`).
- Missing: instruction cache + MMU mapping of flash code (XIP); linker split hot (IRAM) / cold (flash) code with a
  section attribute; cache size/ways/line configuration; cache sync API in general (write-back/invalidate by range is
  only in psram.h); a check that interrupt/USB/RF/DSP hot paths stay in IRAM.
- Why first: every app must fit code + data + stacks in internal RAM (below 0x3FCB0000 when the RF dump engine is
  linked). `libc_test`, `periph_test`, `sys_test` don't build today because of it; `rf_pkt` is full.
- Reference: `bootloader_support/src/esp32s3/bootloader_esp32s3.c`, `esp_mm/` (MMU), `hal/esp32s3/include/hal/cache_ll.h`,
  `hal/mmu_hal.c`, `esp_system/ld/esp32s3/sections.ld.in`.

### 2. Memory map, RTC memory, heap regions — partial (TRM 4)
- Have: internal DRAM heap (`heap.h`), fixed internal SRAM layout (`link.ld`).
- Missing: RTC fast (8 KB) and RTC slow (8 KB) memory sections (`.rtc.data`, `.rtc.noinit`) for data that survives
  deep sleep and resets (a bench app writes 0x50001000 by hand today); PSRAM heap / capability-based malloc (DMA,
  exec, external); unused-IRAM reclaim.
- Reference: `heap/port/esp32s3/memory_layout.c`, `esp_system/ld/esp32s3/memory.ld.in`.

### 3. GDMA — partial (TRM 3)
- Have: 5 channels, linked-list descriptors, peripheral connect, memory-to-memory (sync and async), EOF interrupts.
- Missing: channel priority and weight arbitration; burst settings; external memory (PSRAM) as source/target with
  block-size rules and cache handling; descriptor owner checks; per-channel reset recovery.
- Reference: `hal/esp32s3/include/hal/gdma_ll.h`, `esp_hw_support/dma/gdma.c`.

### 4. Interrupt matrix — done (TRM 9)
- Have: `irq_attach` levels 1-5, shared lines 1-3, naked level 4/5 handlers, internal interrupts, cross-core notify.
- Small gaps: NMI (level 7) for a last-resort handler; routing a source to the other core without running code there.

### 5. Reset and clock — partial (TRM 7, 17)
- Have: CPU 240/160/80/40 MHz, APB, peripheral clock gates/resets (`clk.h`), reset cause per core.
- Missing: RTC slow clock source (RC 136 kHz / external 32 kHz crystal / 8MD256) and its calibration against XTAL;
  RTC fast clock; clock output on pins (CLK_OUT1..3); dynamic frequency scaling; XTAL 32 kHz bring-up.
- Reference: `esp_hw_support/port/esp32s3/rtc_clk.c`, `rtc_time.c`, `hal/esp32s3/include/hal/clk_tree_ll.h`.

### 6. Chip boot, bootloader, OTA — none (TRM 8)
- Have: the ROM loads our image (written at 0x0) straight into RAM; boot banner; partition API.
- Missing: own 2nd-stage loader (needed for XIP and app selection), app partitions + OTA A/B with rollback, image
  header + SHA-256 check (needs 18), boot strapping notes, fast boot path.
- Reference: `bootloader/subproject/main/`, `bootloader_support/src/bootloader_utility.c`, `app_update/esp_ota_ops.c`.

### 7. System timer, timer groups — done (TRM 11, 12)
- `time_us/ms`, `delay_*`, software timers (`swtimer.h`), GPTimer driver. Nothing open.

### 8. Watchdogs — partial (TRM 13, 14)
- Have: TG0 MWDT with a report stage (`wdt.h`). At boot the ROM's watchdogs are switched off (RWDT, TG0/1, super WDT).
- Missing: RTC watchdog (boot-hang and flash-boot protection, chip reset stage); super watchdog (left disabled);
  TG1 MWDT (second core); XTAL32K watchdog (switches to backup clock when the 32 kHz crystal stops).
- Reference: `hal/esp32s3/include/hal/rwdt_ll.h`, `mwdt_ll.h`, `esp_system/int_wdt.c`, `task_wdt/`.

### 9. Brownout, clock-glitch detection — none (TRM 10, 24)
- Have: both reset causes decoded in `sys_reset_cause_str`.
- Missing: brownout detector level + interrupt (cut motors/flush logs before the reset), clock glitch detector.
- Reference: `esp_system/port/brownout.c`, `hal/esp32s3/include/hal/brownout_ll.h`, `esp_hw_support/port/esp32s3/`.

### 10. Low-power — none (TRM 10)
- Missing: light sleep (RAM kept), deep sleep + wake stub in RTC memory, wake sources (timer, ext0/ext1, GPIO, touch,
  UART, ULP, USB), power-domain control and retention, sleep pad configuration, RTC time kept across sleep, measured
  sleep currents vs IDF. RTC GPIO hold through reset already exists.
- Reference: `esp_hw_support/sleep_modes.c`, `port/esp32s3/rtc_sleep.c`, `hal/esp32s3/include/hal/rtc_cntl_ll.h`.

### 11. ULP coprocessors — none (TRM 2)
- Missing: ULP-FSM and ULP-RISC-V loaders, shared RTC memory, wake of the main CPU, RTC I2C for sensors while asleep.
- Reference: `ulp/` component.

### 12. Multi-core — partial (TRM 17)
- Have: core 1 start, `xc_seq` / `xc_fifo` / `xc_spin` / `xc_notify` (`xcore.h`), heap safe from both cores.
- Missing: cross-core function call (run fn on the other core and wait), core stall/unstall and single-core reset API.
- Reference: `esp_system/esp_ipc.c`, `esp_hw_support/cpu.c`.

### 13. Memory protection (PMS) and World Controller — none (TRM 15, 16)
- Missing: IRAM write-protect / DRAM no-execute split, peripheral access permissions, stack guard regions, World
  Controller (secure/non-secure worlds).
- Reference: `esp_hw_support/port/esp32s3/esp_memprot.c`, `hal/esp32s3/include/hal/memprot_ll.h`.

### 14. Debug assist, performance counters, trace — none (Xtensa, TRM 4/17)
- Missing: ASSIST_DEBUG stack-pointer bounds monitor per core (cheap stack-overflow detection) and memory-region
  watchers; Xtensa performance counters (cycles, cache misses, stalls) for profiling; hardware breakpoints and
  watchpoints; TRAX trace notes.
- Reference: `esp_hw_support/port/esp32s3/` (debug assist), `xtensa/` (perfmon, `esp_cpu` watchpoints).

### 15. CPU extensions — partial (TRM 1)
- Have: FPU and PIE enabled at boot; PIE kernels (`dsp.S`); FPU/PIE off inside handlers (a float in an ISR faults).
- Missing: optional lazy save of FPU/PIE state so handlers may use them; hardware loops policy documented.

### 16. eFuse — partial (TRM 5)
- Have: raw bit reads of blocks 1/2, MAC.
- Missing: full field table (chip revision fields, user blocks, key purposes, flash/PSRAM config), virtual eFuses for
  tests, burning only behind an explicit opt-in.
- Reference: `efuse/esp32s3/esp_efuse_table.csv`, `efuse/src/`.

### 17-21. Security accelerators
- 17 RNG (TRM 25) — none: true-random API, entropy notes (needs RF or SAR ADC running for full entropy).
- 18 SHA (TRM 18) — none: SHA-1/224/256/384/512, DMA mode; needed by 6 (image checks) and HMAC/TLS later.
- 19 AES (TRM 19) — done (block + XTS); DMA mode for large CBC/CTR/GCM buffers missing.
- 20 RSA / HMAC / DS (TRM 20-22) — none: big-number accelerator (signatures, secure boot), HMAC from eFuse keys,
  digital-signature peripheral.
- 21 XTS_AES flash/PSRAM encryption, secure boot (TRM 23) — none: irreversible eFuse; plan only until approved.
- Reference: `mbedtls/port/`, `esp_hw_support/hw_random.c`, `hal/esp32s3/include/hal/sha_ll.h`.

## Suggested order (dependencies first)

1. **Cache + MMU + XIP** (1), with the 2nd-stage loader it needs (part of 6) — unblocks RAM for everything else.
2. **RNG + SHA** (17, 18) — small; needed by the boot image check.
3. **OTA A/B with rollback** (rest of 6).
4. **Watchdogs complete + brownout + clock glitch** (8, 9) — field robustness.
5. **RTC clocks + RTC memory** (5, 2) → **low-power sleep + wake** (10) → **ULP** (11).
6. **GDMA advanced** (3): PSRAM DMA, priorities.
7. **Debug assist stack guard + PMS** (14, 13).
8. **Cross-core calls** (12), **eFuse table** (16).
9. **RSA / HMAC / DS, AES DMA** (19, 20).
10. **Flash encryption / secure boot** (21) — plan and approval only.

Each item: read the reference first, own code, IDF sibling on the same board, numbers in `PERFORMANCE.md` /
`RESULTS.md` in the same commit.
