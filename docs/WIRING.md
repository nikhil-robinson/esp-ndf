# Test wiring (devkit)

Board: ESP32-S3 devkit, MAC 24:58:7C:E2:C9:D0
(`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_24:58:7C:E2:C9:D0-if00`).

Verified with official ESP-IDF drivers only (`bench/idf_wiring`), 2026-10-18.

## Jumper wires (pin shorted to pin)

| Pair | Status |
|---|---|
| GPIO10 ↔ GPIO11 | verified: each follows the other driven high and low |
| GPIO12 ↔ GPIO13 | verified |

No other shorts between any probed pin
(0–18, 21, 33–48; 19/20 = USB and 26–32 = flash/PSRAM not probed).

**Rule:** never configure both pins of a pair as outputs at the same time.

## Logic analyzer

Saleae Logic clone, USB id 0925:3881 (fx2lafw, 8 channels).
Needs `sudo apt install sigrok-cli sigrok-firmware-fx2lafw`.

Capture: `sigrok-cli -d fx2lafw --config samplerate=4m --samples N -O csv -o out.csv`
(the first scan after plug-in uploads firmware and re-enumerates; retry once).

Verified 2026-10-18 by capturing `bench/idf_wiring`'s LEDC pattern
(each GPIO a distinct frequency, official ESP-IDF drivers):

| Analyzer channel | GPIO | Seen |
|---|---|---|
| D0 | **GPIO5** | 2.000 kHz, 50 % |
| D1 | **GPIO4** | 1.000 kHz, 50 % |
| D2 | **GPIO6** | 3.000 kHz, 50 % |
| D3–D7 | not connected | idle high |

GPIO7 is not on the analyzer.
