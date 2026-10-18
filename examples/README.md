# esp-ndf examples

Build and flash any of them with `make APP=examples/<name> flash` (add
`PORT=$(BOX3)` for the Box-3). The partition table goes in once with
`make partition`.

| Example | Board | What it shows |
|---|---|---|
| `periph_test` | devkit + jumpers (docs/WIRING.md) | self-test + timing for every driver; the numbers in docs/RESULTS.md come from here |
| `bench` | devkit | 20 kHz loop on each core + 10 kHz timer ISR + USB telemetry (vs FreeRTOS / esp-hal) |
| `irq_latency` | any | interrupt entry latency per level, in CPU cycles |
| `rtc_hold` | devkit, scope on GPIO4 | keep an output driven through a full system reset (pad hold) |
| `storage` | any | NVS boot counter, FAT on wear levelling and SPIFFS through the VFS |
| `box3_lcd` | ESP32-S3-BOX-3 | ILI9342C display over SPI |
| `box3_i2c` | ESP32-S3-BOX-3 | I2C bus scan, sensor/codec ID reads |
| `box3_audio` | ESP32-S3-BOX-3 | ES8311 codec + I2S speaker tones |

Host tools in `tools/`: `mkpart.py` (partition table), `nvstool.py` (NVS
images: gen/parse/keygen), `mfgtool.py` (per-device NVS images),
`fattool.py` (FAT images, optional wear levelling, parse/extract).
