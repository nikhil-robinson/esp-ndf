# esp-ndf TODO: full coverage of the ESP32-S3

Built by checking every chapter of the ESP32-S3 TRM and every ESP-IDF v5.3
component against this repo (October 2026). Rules for each item still apply:
an IDF sibling, measured on the board, faster or equal, and verified accurate.

Legend: `[x]` done and verified · `[~]` partly done · `[ ]` not started ·
`(n/a)` the S3 has no such hardware.

---

## 1. CPU, SIMD and AI acceleration (TRM ch. 1 PIE, Xtensa LX7)

- [ ] **PIE / SIMD support in the toolchain flow**: 128-bit Q registers
      (q0–q7), QACC accumulators, `EE.*` instructions; enable and save the
      Q registers on context switch (coprocessor 1), like the FPU
- [ ] PIE context in interrupts: decide lazy-save or "no PIE in ISR" (as for the FPU now)
- [ ] **DSP library (ESP-DSP equivalent, our own)**, all on PIE:
  - [ ] FFT (radix-2/4, real and complex, int16 + float), windowing
  - [ ] FIR / IIR biquad filters, decimation
  - [ ] dot product, vector add/mul/scale, matrix multiply (int8/int16/float)
  - [ ] convolution / correlation
- [ ] **NN library (ESP-NN equivalent)**: int8 conv2d, depthwise conv,
      fully connected, pooling, ReLU/softmax, quantized add/mul; benchmark
      against ESP-NN on the same models
- [ ] FPU: float in `printf`, fast math helpers (sin/cos/atan2/sqrt for
      control loops), libgcc soft-float double helpers we lack today
- [ ] Hardware loops (`loop` instruction) and zero-overhead loops in hot paths
- [~] FPU in ISRs: currently forbidden (fault); optional lazy save
- [ ] Performance counters (perfmon: cycles, cache misses, stalls) for profiling
- [ ] Debug: hardware breakpoints/watchpoints (stack guard), OCD/JTAG notes

## 2. Memory, cache, external RAM (TRM ch. 4 System and Memory, ch. 23 XTS_AES)

- [ ] **Cache + MMU setup**: execute code from flash (XIP). Keep hot code in
      IRAM and the rest in flash. Lifts the ~400 KB image limit
- [ ] **PSRAM** quad + octal: MSPI timing tuning, MMU mapping, 2–8 MB heap,
      DMA to PSRAM, cache write-back control
- [ ] Octal / 120 MHz flash modes (OPI), flash timing tuning
- [ ] **Heap allocator** (malloc/free) with capabilities: internal/DMA/PSRAM/exec
- [ ] RTC fast/slow memory (8 KB + 8 KB): data that survives deep sleep
- [ ] Memory protection (TRM ch. 15 PMS, ch. 16 World Controller): IRAM
      write-protect, stack guards, peripheral access control
- [ ] Stack overflow detection (per core)
- [ ] External memory encryption (XTS_AES): flash/PSRAM encryption (irreversible eFuse; see STORAGE_SECURITY.md)

## 3. System, reset, clocks, boot (TRM ch. 7, 8, 9, 13, 14, 17, 24)

- [~] Clocks: CPU 240/160/80/40 done; **todo**: RTC slow clock source
      (RC 136 kHz / 32 kHz crystal / 8MD256), calibration, clock output on pins (CLK_OUT)
- [ ] XTAL 32 kHz support + **XTAL32K watchdog** (ch. 14)
- [ ] **Watchdogs** (ch. 13): MWDT (timer groups), RTC WDT, interrupt-watchdog
      and loop-watchdog equivalents; today all are off at boot
- [x] **Reset reason** per core (`sys_reset_cause`); [ ] wake cause (with sleep)
- [ ] **Brownout detector**, with an ISR so motors can be cut before the reset
- [ ] **Clock glitch detection** (ch. 24)
- [ ] **Fault handling**: backtrace, register dump, reset-safe crash record
      (core dump to flash), assert
- [ ] Software restart API, controlled reset of a single core
- [ ] Interrupt matrix extras: per-core routing API, interrupt priorities documented, NMI (level 7) use
- [~] Dual core: core 1 start done; **todo**: spinlocks, cross-core calls
      (IPC), inter-core FIFO/mailbox, per-core interrupt allocation
- [ ] System timer API (ch. 11): 64-bit µs time base, alarms, `esp_timer`-like
      software timers (one-shot/periodic, ISR dispatch)
- [ ] System time / RTC time kept across sleep, `gettimeofday`-like
- [ ] Boot: own 2nd-stage loader option, app partitions, **OTA** (A/B with
      rollback), image header + SHA-256 check (needs SHA)
- [x] Chip info: revision, package, MAC, flash id/size (`sys_info`); [ ] PSRAM size (with PSRAM)
- [ ] ROM-free: confirm nothing still depends on ROM functions after the above

## 4. Power management (TRM ch. 10 RTC_CNTL, ch. 2 ULP)

- [ ] **Light sleep** (CPU paused, RAM kept) with automatic entry when idle
- [ ] **Deep sleep** + wake stub in RTC RAM
- [ ] Wake sources: timer, ext0/ext1 GPIO, any GPIO (light sleep), touch,
      UART, ULP, USB
- [ ] Dynamic frequency scaling (power-management locks)
- [ ] Peripheral power domains, retention DMA (ch. 10.5)
- [ ] **ULP coprocessor**: ULP-RISC-V and ULP-FSM: loader, shared RTC memory,
      wake main CPU; RTC I2C controller (ch. 2.7) for sensors while asleep
- [~] RTC GPIO: done (incl. hold through reset); **todo**: sleep pad
      configuration (sleep output/input/pulls per pad), digital pad hold (GPIO22+)
- [ ] Measured sleep currents vs IDF

## 5. Security and crypto (TRM ch. 5, 18–25)

- [x] AES (block + XTS)
- [ ] AES DMA mode (CBC/CTR/GCM, large buffers)
- [ ] **SHA** 1/224/256/384/512 (needed for OTA image checks, HMAC, TLS later)
- [ ] **RSA / big-number accelerator** (modexp, modmul): signatures, secure boot
- [ ] **HMAC** peripheral (NVS key derivation from eFuse, JTAG unlock)
- [ ] **Digital Signature (DS)** peripheral
- [ ] **RNG** API (true random with RF/SAR entropy notes)
- [ ] **eFuse API** (ch. 5): read all fields, custom user fields, virtual
      eFuses for tests; burning only behind an explicit opt-in
- [ ] Secure boot v2 (irreversible eFuse; plan only until approved)
- [ ] Flash encryption (irreversible eFuse; plan only until approved)
- [x] NVS encryption (XTS); [ ] HMAC key scheme on the device

## 6. Peripherals: remaining modes of finished drivers

- [~] **I2S**: standard mode done; **todo**: TDM (Box-3 ES7210 mic array),
      PDM TX/RX (PDM mics), RX from codec, full-duplex, 8/24/32-bit, LCD/ADC modes
- [~] **SPI master**: 1-bit done; **todo**: dual/quad/octal (QSPI/OPI)
      transfers, half-duplex command/address/dummy phases, SPI3 as master,
      CS timing setup/hold, DMA chained large transfers (> 4 KB descriptors)
- [~] **SPI slave**: full duplex done; **todo**: half-duplex slave (SPI_SLAVE_HD), segmented transfer
- [~] **UART**: done; **todo**: RS485 (half-duplex, collision detect), IrDA,
      hardware flow control test, pattern/AT-command detection, UART wake-up, UHCI DMA
- [~] **I2C**: master/slave done; **todo**: 10-bit address, clock stretching
      timeout tuning, SCL glitch filter config, bus-clear/recovery, multi-master arbitration loss handling
- [~] **RMT**: done incl. carrier/filter; **todo**: TX loop count + sync of several channels, RX demodulation, DMA mode (ch 3/7), encoders (WS2812 driver as example)
- [~] **MCPWM**: done incl. capture/fault/sync; **todo**: carrier (chopper) module, event-triggered fault actions per operator, capture prescaler
- [~] **PCNT**: done; **todo**: threshold/zero-cross events as callbacks, step-notify API, decoder for quadrature with index
- [~] **LEDC**: done incl. fade; **todo**: gamma curves, channel phase sync API, sleep retention
- [~] **GPTimer**: done; **todo**: capture-on-event, ETM-like linking is (n/a on S3)
- [~] **Touch**: raw read done; **todo**: filtering/denoise, waterproof
      (shield pad), proximity mode, baseline tracking, threshold interrupts, touch wake
- [~] **ADC**: one-shot + continuous done; **todo**: IIR filters and
      threshold monitors (hardware), ADC2 one-shot arbitration notes, continuous on ADC2 (n/a in DMA mode on S3)
- [~] **GDMA**: done; **todo**: priorities, ETM (n/a), burst/ext-mem rules once PSRAM is up, weight arbitration
- [~] **LCD**: SPI panel done; **todo**: **i80 parallel bus** (8/16-bit) and **RGB panels** via LCD_CAM, more panel drivers (ST7789, GC9A01), touch controller (Box-3 TT21100/GT911)
- [~] **TWAI**: done; **todo**: bus-off recovery API, error-passive callbacks, alert interrupts

## 7. Peripherals not started

- [ ] **Camera** (LCD_CAM camera, DVP 8/16-bit) + JPEG/YUV handling example
- [ ] **USB OTG** (ch. 32): device stack (CDC-ACM, HID, MSC, vendor), host
      (CDC, MSC, HID), OTG role switch
- [~] USB Serial/JTAG: console done; **todo**: RX path (input), JTAG-only mode, disconnect detection
- [ ] **SD/MMC host** + **SD SPI host** + SD/MMC protocol driver (waiting for a card)
- [ ] **Temperature sensor** (ch. 39.4)
- [ ] Analog comparator, DAC, SDIO slave, Ethernet MAC, ISP, JPEG codec, PPA, parallel IO (n/a on S3)

## 8. Runtime and libraries (IDF: esp_system, newlib, log, ringbuf, console, cxx...)

- [ ] libc completeness: `snprintf` with float, `strtol/strtoul/strtof`,
      `qsort`, `memmove` audit, `time` functions
- [ ] Logging with levels/tags, timestamps, optional compile-out
- [ ] Ring buffers / queues shared ISR ↔ main and core ↔ core (lock-free SPSC)
- [ ] Event dispatch (lightweight, no RTOS)
- [ ] Console input / command line (REPL) over USB serial
- [ ] Unit test framework on the device (our CHECK/METRIC grown into a runner) + host tests
- [ ] C++ support: global constructors, `new/delete`, no exceptions/RTTI option
- [ ] Optional cooperative scheduler / task API for apps that want one (still no RTOS in the hot path)
- [ ] gdbstub / JTAG debugging guide, app trace (fast logging over JTAG)
- [x] Configuration system: `ndf_config.h` (run-time options, app override) + `app.mk` (build options) + `make menuconfig` (tools/ndfconfig: nested, searchable, themed, CLI mode) + docs/CONFIG.md generated from one schema
- [x] System API (`system.h`): chip revision, MAC, flash id/size/read path, clocks, reset cause per core, restart, boot banner
- [x] UART0 console option; storage limits (VFS/FAT/SPIFFS/NVS) configurable; config changes trigger a rebuild
- [ ] More menuconfig options as features land (log level, watchdogs, PSRAM, ...); IDF option inventory: docs/IDF_CONFIG_INVENTORY.md

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
- [ ] Long-run soak test (24 h) of the full suite with power-cut injection
- [ ] Per-driver API docs + porting guide (rule 3: other chip families)
