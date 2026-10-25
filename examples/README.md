# esp-ndf examples

Each example is a folder with its own `Makefile` (three lines) and sources: `cd examples/<area>/<name> && make flash monitor`
(add `PORT=$(BOX3)` for the Box-3). The partition table goes in once with `make partition`. Configure with `make menuconfig`
(writes `ndf_config.h` / `app.mk` in that folder; all options: [docs/CONFIG.md](../docs/CONFIG.md)). Tests are not examples:
see [docs/TESTING.md](../docs/TESTING.md).

## System and timing

| Example | What it shows |
|---|---|
| [`bench`](system/bench) | Benchmark on esp-ndf: 20 kHz loop on both cores, 10 kHz TIMG0 ISR on core 0, 1 kHz USB telemetry. |
| [`rtc_hold`](system/rtc_hold) | Keep an output (GPIO4, e.g. a motor driver enable) high through a full system reset with an RTC pad hold; the rebooted firmware takes it over and drives it low 300 ms... |
| [`sysinfo`](system/sysinfo) | System info + configuration check: the banner (NDF_BOOT_BANNER) prints before main(); here we confirm the overrides took effect. |

## Storage (NVS, FAT, SPIFFS)

| Example | What it shows |
|---|---|
| [`storage`](storage/storage) | Storage stack in one place: NVS (boot counter + calibration blob), FAT on wear-levelled flash and SPIFFS, both reached through the VFS. |

## USB device

| Example | What it shows |
|---|---|
| [`usb_cdc`](usb/usb_cdc) | USB OTG full-speed CDC-ACM device test (usb.h). |
| [`usb_msc`](usb/usb_msc) | USB mass storage: a 128 KB RAM disk formatted FAT (serial 1234-ABCD) with HELLO.TXT, served for an OTG window of `secs` seconds on "msc <secs>"; "hid <secs>" runs a... |

## Bluetooth LE (own link layer, no vendor code)

| Example | What it shows |
|---|---|
| [`ble_adv`](bluetooth/ble_adv) | Hardware BLE advertising, no vendor code: the S3's link-layer core sends ADV_NONCONN_IND (CRC, whitening, GFSK by the hardware) on channel 37 every 20 ms; the CPU... |
| [`ble_conn`](bluetooth/ble_conn) | BLE peripheral on the S3's own link layer, no vendor code: connectable advertising, the connection (CONNECT_IND, CSA#1 hopping, LL control by ble_hw_ll_control) and a... |
| [`ble_scan`](bluetooth/ble_scan) | Hardware BLE scan, no vendor code: our PHY init, then the S3's own link-layer core receives advertising PDUs on channel 37 by itself (demodulation, access address,... |

## Radio (Wi-Fi, 802.15.4, packet engine; see docs/RF_API.md)

| Example | What it shows |
|---|---|
| [`rf_154`](rf/rf_154) | 4.3: transmit an IEEE 802.15.4 O-QPSK frame as a DAC waveform (rf.h: rf_154_build + rf_tx_play). |
| [`rf_cal`](rf/rf_cal) | rf_cal: the calibration store, driven by line commands (tools/rf_cal_test.py). open | close | erase | tone khz offset_khz | dcres (RX DC residual at gain indices... |
| [`rf_pkt`](rf/rf_pkt) | rf_pkt: the hardware packet engine (rf.h) driven by line commands, for tools/rf_pkt_test.py (two boards). send mhz rate sync flags len count     payload 00 01 02 ...;... |
| [`rf_test`](rf/rf_test) | Blob-free radio on esp-ndf: our PHY init (replay), PLL, receive with the dump engine at forced gains, then DAC playback + capture together. |
| [`rf_wifi`](rf/rf_wifi) | 4.2: raw 802.11 over the Wi-Fi MAC from our own code (rf.h: rf_wifi_*), no vendor stack. |
| [`rfctl`](rf/rfctl) | rfctl: the radio driven by line commands over the console, for host-scripted measurements (tools/rfhost.py). |
| [`rfctl_uart`](rf/rfctl_uart) |  |

## Radio bring-up and measurement experiments

| Example | What it shows |
|---|---|
| [`rf_afc`](rf_lab/rf_afc) | AFC: lock our LO to a reference transmitter (D0: tone at LO 2437 MHz + 2 MHz) with the public RF API. |
| [`rf_dma`](rf_lab/rf_dma) | A.4: moving the IQ stream out of the dump banks with GDMA while the engine runs. |
| [`rf_dumpinfo`](rf_lab/rf_dumpinfo) | Dump engine limits: capture rate per control-bit combination (time to done for n words), playback position registers, config-register effects. |
| [`rf_hop`](rf_lab/rf_hop) | Retune speed: full rf_pll_set_khz vs rf_pll_hop with a cached cap, and the LO settling seen over the air. |
| [`rf_modem`](rf_lab/rf_modem) | Bench: map the BT modem block (0x60011000) below the BLE link layer. |
| [`rf_ota`](rf_lab/rf_ota) | Over-the-air check of our transmit path, two boards at 2437 MHz. |
| [`rf_ota_uart`](rf_lab/rf_ota_uart) |  |
| [`rf_probe`](rf_lab/rf_probe) | Capture while the DAC plays: where does the capture data go? |
| [`rf_range`](rf_lab/rf_range) | PLL tuning range (700 MHz .. 3.4 GHz) and the RC calibration code read back after init. |
| [`rf_state`](rf_lab/rf_state) | Persistent-state hunt: snapshot RTC_CNTL, SENS and every analog register (blocks 0x61..0x6D, regs 0..0x3F) after our init + TX on, then do an RTC_CNTL system reset... |
| [`rf_state_uart`](rf_lab/rf_state_uart) | Persistent-state hunt: snapshot RTC_CNTL, SENS and every analog register (blocks 0x61..0x6D, regs 0..0x3F) after our init + TX on, then do an RTC_CNTL system reset... |
| [`rf_txsweep`](rf_lab/rf_txsweep) | TX gain-row field sweep, measured over the air. |

## ESP32-S3-BOX-3 board

| Example | What it shows |
|---|---|
| [`box3_audio`](box3/box3_audio) | BOX-3 speaker: ES8311 codec (I2C 0x18) fed by I2S0 as master, MCLK = 256 fs. |
| [`box3_i2c`](box3/box3_i2c) | BOX-3 internal I2C bus scan (SCL 18, SDA 8, board pull-ups) at 100 k and 400 k. |
| [`box3_lcd`](box3/box3_lcd) | ESP32-S3-BOX-3 display demo: color bars, gradient, moving block. |
| [`box3_mic`](box3/box3_mic) | BOX-3 microphones end to end: ES8311 plays 1 kHz through the speaker while the ES7210 (I2C 0x40) records MIC1/MIC2 on I2S0 RX (full duplex with the speaker TX: MCLK... |
| [`box3_touch`](box3/box3_touch) | BOX-3 GT911 touch controller: id, resolution, report reads (touch the screen to see points). |

