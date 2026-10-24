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
- [x] Fast math (sin/cos/atan2/exp/log/rsqrt) + Q15/Q31, soft-double runtime; [~] float in printf (check %f path)
- [x] Zero-overhead loops in hot paths (all PIE/FPU kernels)
- [x] Performance counters, watchpoints, hardware stack guard (examples/debug_test); [ ] gdbstub / app-trace guide (section 8)

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

- [x] Clocks: CPU 240/160/80/40, RTC slow clock select + calibration; [ ] CLK_OUT pins (no use yet)
- [~] XTAL 32 kHz + XTAL32K watchdog: written, not verified (no crystal on either board)
- [x] Watchdogs: TG0/TG1 (report + reset), RTC WDT, super watchdog
- [x] Reset reason per core (kept across the warm stage); [ ] wake cause (with sleep, D1)
- [x] Brownout detector (interrupt or hardware reset mode), clock-glitch reset
- [x] Fault handling with DWARF backtrace; [ ] reset-safe crash record / core dump to flash
- [x] Software restart, core 1 stall/restart (xc_stall, cpu1_start)
- [~] Interrupt matrix: per-core routing, priorities, line sharing done; [ ] NMI (level 7) use
- [x] Dual core: spinlocks, seq/fifo, cross-core notify and calls (24x esp_ipc)
- [x] System timer, software timers
- [ ] System time kept across sleep, gettimeofday-like (with D1)
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

- [ ] **Camera** (LCD_CAM camera, DVP 8/16-bit) + JPEG/YUV handling example
- [ ] **USB OTG** (ch. 32): device stack (CDC-ACM, HID, MSC, vendor), host
      (CDC, MSC, HID), OTG role switch
- [~] USB Serial/JTAG: console out + input (`console_getc`) done; **todo**: JTAG-only mode, disconnect detection
- [ ] **SD/MMC host** + **SD SPI host** + SD/MMC protocol driver (waiting for a card)
- [x] **Temperature sensor**: eFuse trim, auto range, RC_FAST clock as IDF (within 0.6 C of IDF, 3.2x faster read); [ ] threshold interrupt
- [ ] Analog comparator, DAC, SDIO slave, Ethernet MAC, ISP, JPEG codec, PPA, parallel IO (n/a on S3)

## 8. Runtime and libraries (IDF: esp_system, newlib, log, ringbuf, console, cxx...)

- [x] libc: `snprintf` family with %f/%e/%g, `strtol/ul/ll/ull`, `strtof/strtod` (float precision), `qsort`, `bsearch`, `atoi`;
      float runtime (`__divsf3`, `sqrtf`, conversions) IEEE-exact; double arithmetic (asm fast paths, faster than IDF ROM);
      correctly rounded `strtod`/`strtof`; exact `printf` (any precision, %a); [ ] `time()`/`gmtime` functions, libm (sinf, expf, ...)
- [x] Logging with levels/tags, timestamps, compile-out (`log.h`), `NDF_ASSERT`
- [x] Lock-free SPSC FIFO (`XC_FIFO`, ISR ↔ main and core ↔ core); [x] byte ring buffer with variable-length items (`ringbuf.h`, zero-copy)
- [ ] Event dispatch (lightweight, no RTOS)
- [ ] Console input / command line (REPL) over USB serial
- [ ] Unit test framework on the device (our CHECK/METRIC grown into a runner) + host tests
- [ ] C++ support: global constructors, `new/delete`, no exceptions/RTTI option
- [ ] Optional cooperative scheduler / task API for apps that want one (still no RTOS in the hot path)
- [ ] gdbstub / JTAG debugging guide, app trace (fast logging over JTAG)
- [x] Configuration system: `ndf_config.h` (run-time options, app override) + `app.mk` (build options) + `make menuconfig` (tools/ndfconfig: nested, searchable, themed, CLI mode) + docs/CONFIG.md generated from one schema
- [x] System API (`system.h`): chip revision, MAC, flash id/size/read path, clocks, reset cause per core, restart, boot banner
- [x] UART0 console option; storage limits (VFS/FAT/SPIFFS/NVS) configurable; config changes trigger a rebuild
- [ ] More menuconfig options as features land (watchdogs, PSRAM, ...); IDF option inventory: docs/IDF_CONFIG_INVENTORY.md

## 9. Storage (mostly done)

- [x] NVS (+ encryption, boot reader, host tools), partition API, FAT + LFN,
      wear levelling, SPIFFS, VFS, block-device layer, FAT/NVS/mfg host tools
- [ ] FAT: rename, directory rename/move, timestamps from system time, FAT32 in `fattool mkfat`
- [ ] SPIFFS: rename, `spiffsgen`-compatible host image builder
- [ ] SD card as block device (with the SD host)
- [ ] LittleFS (power-safe filesystem: better than FAT for logs on a robot)

## 10. Wireless (rule 1: after peripherals)

- [ ] Wi-Fi (station/AP), BLE 5: own stack per rule 3; PHY calibration and
      coexistence need research (RF blobs are the hard part)
- [ ] Networking: TCP/IP, UDP, MQTT/HTTP clients, OTA over Wi-Fi
- [ ] ESP-NOW (low-latency robot links)

## 11. Verification and documentation backlog

- [ ] IDF siblings still missing: ADC continuous, LCD, RTC hold through reset
- [ ] Box-3 microphones (ES7210 over I2S TDM) end to end
- [ ] Long-run soak test (24 h) of the full suite with power-cut fault testing
- [ ] Per-driver API docs + porting guide (rule 3: other chip families)
