# esp-ndf TODO: full coverage of the ESP32-S3

Built by checking every chapter of the ESP32-S3 TRM and every ESP-IDF v5.3
component against this repo (October 2026). Rules for each item still apply:
an IDF sibling, measured on the board, faster or equal, and verified accurate.

Legend: `[x]` done and verified · `[~]` partly done · `[ ]` not started ·
`(n/a)` the S3 has no such hardware.

---

## 1. CPU, SIMD and AI acceleration (TRM ch. 1 PIE, Xtensa LX7)  - details: docs/SYSTEM_TODO.md A1-A8

- [x] PIE / SIMD in the toolchain flow, enabled at boot; instruction cookbook docs/PIE.md
- [x] PIE and FPU in interrupt handlers: lazy save, any level, nested (examples/pie_isr, fpu_isr)
- [x] DSP library (ESP-DSP equivalent): FFT f32/rfft/s16, windows, FIR/FIRD, biquad, vector, matrix, conv/corr,
      DCT, Kalman - 1.17-33x ESP-DSP (examples/dsp_lib)
- [x] NN library (ESP-NN equivalent): conv/dwconv/fc/pool/add/mul/relu6/softmax bit-exact, tiny model 1.17x
- [x] Fast math (sin/cos/atan2/exp/log/rsqrt) + Q15/Q31, soft-double runtime; [x] float in printf (%f/%e/%g exact, libc_test)
- [x] Zero-overhead loops in hot paths (all PIE/FPU kernels)
- [x] Performance counters, watchpoints, hardware stack guard (examples/debug_test); [x] gdb guide (docs/DEBUG.md)

## 2. Memory, cache, external RAM (TRM ch. 4, ch. 23 XTS_AES)  - details: SYSTEM_TODO B1-B5

- [x] Cache + MMU, XIP from flash (lib / all modes)
- [x] PSRAM octal 8 MB (C9:D0) / 16 MB (BOX-3): timing, MMU, heap, DMA to PSRAM, cache write-back
- [x] 120 MHz flash with timing tuning; [ ] octal (OPI) flash and 4-byte addressing on real hardware (no such chip here)
- [x] Heap with capabilities (internal / PSRAM / exec)
- [x] RTC fast/slow memory sections (RTC_DATA / BSS / NOINIT)
- [x] Memory protection (PMS split, monitor, lock); World Controller (n/a: one world used)
- [x] Stack overflow detection per core (SP monitor from boot)
- [ ] External memory encryption (XTS_AES): plan in docs/SECURITY_PLAN.md, needs eFuse approval

## 3. System, reset, clocks, boot (TRM ch. 7-9, 13, 14, 17, 24)  - details: SYSTEM_TODO C1-C7

- [x] Clocks: CPU 240/160/80/40, RTC slow clock select + calibration; [x] CLK_OUT pins (`clk_out`: XTAL / RC fast / RC slow measured on GPIO41; USB pins refused)
- [~] XTAL 32 kHz + XTAL32K watchdog: written, not verified (no crystal on either board)
- [x] Watchdogs: TG0/TG1 (report + reset), RTC WDT, super watchdog
- [x] Reset reason per core (kept across the warm stage); [ ] wake cause (with sleep, D1)
- [x] Brownout detector (interrupt or hardware reset mode), clock-glitch reset
- [x] Fault handling with DWARF backtrace; [x] crash record in RTC memory (`sys_last_crash`, examples/crash_test); [ ] core dump to flash
- [x] Software restart, core 1 stall/restart (xc_stall, cpu1_start)
- [~] Interrupt matrix: per-core routing, priorities, line sharing done; [x] NMI (level 7): `irq_attach_nmi`, gpt_alarm level 7; fires inside irq_lock (runtime_test)
- [x] Dual core: spinlocks, seq/fifo, cross-core notify and calls (24x esp_ipc)
- [x] System timer, software timers
- [x] gettimeofday / settimeofday / clock_gettime (lib/time.c); [ ] wall time kept across sleep / reset (with D1)
- [x] Own 2nd-stage loader, warm boot 0.9 ms, OTA A/B + rollback, SHA-256 image check
- [x] Chip info incl. PSRAM size
- [x] ROM-free (no ROM calls in our code)

## 4. Power management (TRM ch. 10 RTC_CNTL, ch. 2 ULP)  - details: SYSTEM_TODO D1-D3

- [~] Light sleep: written, wakes proven on the analyzer, USB link does not come back - parked (breaks the board link)
- [~] Deep sleep: written, not verified - parked
- [ ] Wake sources beyond timer/ext0/ext1, sleep pad configuration, measured currents vs IDF - with D1
- [x] Dynamic frequency scaling (CPU locks, really switch while running)
- [ ] Peripheral power domains, retention - with D1
- [x] ULP-RISC-V + ULP-FSM loader / run / stop / shared memory (3.5x IDF start); [ ] wake main CPU (D1);
      [ ] RTC I2C (pins fixed to GPIO0-3: needs a sensor wired there)
- [~] RTC GPIO incl. hold through reset; [ ] sleep pad config, digital pad hold (GPIO22+)

## 5. Security and crypto (TRM ch. 5, 18-25)  - details: SYSTEM_TODO E1-E9

- [x] AES block/XTS + ECB/CBC/OFB/CTR/CFB with DMA, AES-GCM
- [x] SHA 1/224/256/384/512 (CPU + DMA)
- [x] RSA / big-number accelerator
- [x] P-256 ECDSA/ECDH, X25519 (software, constant time)
- [x] RNG (ChaCha20 over hardware entropy)
- [x] eFuse API: all fields by name (read); burning only behind an explicit opt-in
- [ ] HMAC, Digital Signature: plan in docs/SECURITY_PLAN.md (eFuse key burn needs approval)
- [ ] Secure boot v2, flash encryption: plan only (irreversible, approval + spare board)
- [x] NVS encryption (XTS); [ ] HMAC key scheme on the device (with HMAC)

## 6. Peripherals: remaining modes of finished drivers

- [x] **I2S**: standard, TDM (2-16 slots, mask), PDM TX/RX (I2S0), full duplex, 8/16/24/32-bit;
      [ ] PDM SNR 33 vs IDF 38 dB, [ ] ES7210 TDM mics on BOX-3 (section 9), [ ] LCD/ADC/camera modes (with LCD_CAM)
- [x] **SPI master**: full duplex, half duplex cmd/addr/dummy on 1/2/4/8 lines (octal SPI2), SPI3 master,
      CS setup/hold, DMA chains to 32 KB per call (split above); [ ] DTR (DDR) phases, [ ] octal read (no octal slave to test)
- [~] **SPI slave**: full duplex + **half-duplex buffer mode** (WRBUF/RDBUF 1/2/4 lines) done; [ ] HD DMA (WRDMA/RDDMA), segmented transfer
- [x] **UART**: RS485 half duplex (DE) + collision detect, IrDA, HW flow control, pattern/AT detection;
      [ ] UHCI DMA (not needed so far: IRQ rings already at wire speed at 5 Mbaud), [ ] UART wake-up (with sleep)
- [x] **I2C**: master/slave, 10-bit address (both), SCL timeout API, glitch filter API, bus clear (IDF order),
      arbitration loss (waits for the winner, retry), clean controller rebuild after errors
- [x] **RMT**: carrier/filter, TX loop count, multi-channel sync, RX demodulation, DMA (TX/RX ch 3), WS2812 encoder
- [x] **MCPWM**: capture/fault/sync, carrier (chopper), fault pins with one-shot / cycle-by-cycle actions per operator, capture prescaler (measured mapping), live re-timing
- [x] **PCNT**: 32-bit gap-free count, watch points (thres0/1, zero, limit) callbacks, step notify (gap-free), quadrature index (GPIO IRQ)
- [x] **LEDC**: fade (fixed: 1 duty per step), gamma fade (segmented, perceptually linear), phase (hpoint) per channel; [ ] sleep retention (with sleep)
- [x] **GPTimer**: done; capture-on-event / ETM: n/a on S3
- [~] **Touch**: raw, hardware filter + baseline, threshold active/inactive events, proximity; [ ] denoise (reference
      channel setup), [ ] waterproof (needs a shield electrode), [ ] touch wake (with sleep)
- [x] **ADC**: one-shot + continuous, hardware IIR filters and threshold monitors (correct channel encoding);
      ADC2 continuous: n/a on S3; [ ] ADC2 one-shot arbitration notes (with Wi-Fi)
- [x] **GDMA**: priorities, PSRAM (burst/ext-mem rules), owner check, error recovery; ETM n/a; weight arbitration n/a (S3 has priority only)
- [~] **LCD**: SPI panel (ILI9341 family), **i80 8/16-bit bus** with DMA, **RGB panel timing** (LCD_CAM), GT911 touch
      (BOX-3); [ ] ST7789 / GC9A01 init tables (no such panel here), [ ] RGB on a real panel
- [x] **TWAI**: alerts (tx, arb lost, bus error, warn, passive/active, bus-off, recovered), state, bus-off recovery + restart

## 7. Peripherals not started

- [x] **Camera** (DVP 8/16-bit, VSYNC frames, XCLK out), verified by an LCD->camera loop; [ ] sensor example (no camera module here), JPEG/YUV
- [~] **USB OTG** (ch. 32): device CDC-ACM (1.32x / 1.97x IDF), **MSC** (BOT + SCSI over any bdev: kernel enumerates,
      READ CAPACITY / READ(10) data proven by udev's FAT probe, 0 SCSI errors), **HID** (vendor 64-byte reports: report
      descriptor parsed by the kernel); [ ] HID/MSC data transfer test from the host (needs root for hidraw / sdX),
      [ ] composite device, [ ] host mode, role switch.
      [x] OTG guard: usb_dev_start arms the RTC watchdog (RTC reset), usb_dev_poll feeds it, usb_dev_stop disarms;
      proof that the RTC reset clears RTC_CNTL and the console returns: examples/usb_safety_proof + tools/usb_safety_proof.py;
      CDC test re-run on C9:D0 with the guard (1152 / 1005 KB/s, 0 errors)
- [~] USB Serial/JTAG: console out + input (`console_getc`) done; **todo**: JTAG-only mode, disconnect detection
- [ ] **SD/MMC host** + **SD SPI host** + SD/MMC protocol driver (waiting for a card)
- [x] **Temperature sensor**: eFuse trim, auto range, RC_FAST clock as IDF (within 0.6 C of IDF, 3.2x faster read); threshold interrupt: n/a on S3 (C6+ hardware)
- [ ] Analog comparator, DAC, SDIO slave, Ethernet MAC, ISP, JPEG codec, PPA, parallel IO (n/a on S3)

## 8. Runtime and libraries (IDF: esp_system, newlib, log, ringbuf, console, cxx...)

- [x] libc: `snprintf` family with %f/%e/%g, `strtol/ul/ll/ull`, `strtof/strtod` (float precision), `qsort`, `bsearch`, `atoi`;
      float runtime (`__divsf3`, `sqrtf`, conversions) IEEE-exact; double arithmetic (asm fast paths, faster than IDF ROM);
      correctly rounded `strtod`/`strtof`; exact `printf` (any precision, %a); libm (lib/math.c, mathf.c);
      [x] `time`/`gettimeofday`/`settimeofday`/`clock_gettime`, `gmtime_r`/`mktime`/`timegm`/`strftime` (UTC, 11-75x newlib)
- [x] Logging with levels/tags, timestamps, compile-out (`log.h`), `NDF_ASSERT`
- [x] Lock-free SPSC FIFO (`XC_FIFO`, ISR ↔ main and core ↔ core); [x] byte ring buffer with variable-length items (`ringbuf.h`, zero-copy)
- [x] Event dispatch (`event.h`): post from ISR / either core, handlers by id or any, drops counted (24x esp_event post)
- [x] Command line (`cmd.h`): compile-time CMD() registry, line editing, quotes, help; host test tools/repl_test.py
- [x] Unit test runner (`unit.h`): TEST() registry, UASSERT/UASSERT_EQ, per-test cycles, "=== unit: P/N passed ==="; [ ] port periph_test onto it
- [x] C++ support: `.cpp` in apps, global constructors (init_priority) + `__attribute__((constructor))`, new/delete on the heap, header-only std (array, algorithm), no exceptions/RTTI (examples/cpp_test)
- [x] Cooperative tasks (`coop.h`): spawn / yield / sleep, CALL0 context (6 registers), hardware stack guard follows the task
- [x] JTAG debugging guide + `tools/gdb.sh` (docs/DEBUG.md, verified: breakpoint, backtrace, variables); [ ] app trace over JTAG
- [x] Configuration system: `ndf_config.h` (run-time options, app override) + `app.mk` (build options) + `make menuconfig` (tools/ndfconfig: nested, searchable, themed, CLI mode) + docs/CONFIG.md generated from one schema
- [x] System API (`system.h`): chip revision, MAC, flash id/size/read path, clocks, reset cause per core, restart, boot banner
- [x] UART0 console option; storage limits (VFS/FAT/SPIFFS/NVS) configurable; config changes trigger a rebuild
- [ ] More menuconfig options as features land (watchdogs, PSRAM, ...); IDF option inventory: docs/IDF_CONFIG_INVENTORY.md

## 9. Storage (mostly done)

- [x] NVS (+ encryption, boot reader, host tools), partition API, FAT + LFN,
      wear levelling, SPIFFS, VFS, block-device layer, FAT/NVS/mfg host tools
- [x] FAT: rename + move (files, directories with '..' fix-up), create/write/access timestamps from time(), `fat_mtime`; [x] FAT32 in `fattool mkfat` (fsck.fat clean, round trip: tools/test_fattool.sh)
- [x] SPIFFS: rename; host image builder tools/spiffsgen.py (image read by esp-ndf and by IDF esp_vfs_spiffs: bench/idf_spiffsgen)
- [ ] SD card as block device (with the SD host)
- [ ] LittleFS (power-safe filesystem: better than FAT for logs on a robot)

## 10. Wireless (rule 1: after peripherals)

- [ ] Wi-Fi (station/AP), BLE 5: own stack per rule 3; PHY calibration and
      coexistence need research (RF blobs are the hard part)
- [ ] Networking: TCP/IP, UDP, MQTT/HTTP clients, OTA over Wi-Fi
- [ ] ESP-NOW (low-latency robot links)

## 11. Verification and documentation backlog

- [x] IDF siblings for ADC continuous (jitter 2 vs 76 cycles), LCD (i80 2.76x), RTC hold (both hold cleanly) through reset
- [x] Box-3 microphones end to end (ES7210 normal I2S, full duplex with the speaker: 38 / 27 dB 1 kHz SNR); [ ] ES7210 4-slot TDM mode
- [~] Soak: examples/soak + tools/soak.py (SHA, AES-GCM, PSRAM 4 MB, FAT, events, coop, temp; WDT, crash records); 24 h run on BOX-3 started 2026-10-25 (bench/soak/data); [ ] power-cut fault testing (needs a switched supply)
- [x] Fixed: 16 MB PSRAM mapping overwrote the XIP code MMU entries (shared 512-entry table): psram_init maps free entries only (BOX-3 + XIP: 15 MB)
- [x] API index (docs/API.md, from the headers) + porting guide (docs/PORTING.md); [ ] bring-up of a second chip
