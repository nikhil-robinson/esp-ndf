# SD card pull-up requirements (ESP32-S3)

What a board needs before the SDMMC host or the SD-SPI host can work
reliably. The drivers for both hosts are still pending; they will be written
and verified once a card is in the Box-3 dock.

## Required pull-ups

| Line | SD (SDMMC) mode | SPI mode | Pull-up |
|---|---|---|---|
| CLK | clock | SCK | none |
| CMD | command, open-drain during identification | MOSI | **10 kΩ to 3.3 V** |
| DAT0 | data | MISO | **10 kΩ** |
| DAT1 | data (4-bit) / SDIO interrupt | unused | **10 kΩ**, also in 1-bit mode |
| DAT2 | data (4-bit) | unused | **10 kΩ**, also in 1-bit mode |
| DAT3 | data (4-bit), card detect at power-up | CS | **10 kΩ** |

- The SD specification requires pull-ups on CMD and on every DAT line, used
  or not. A floating DAT1 or DAT2 in 1-bit mode can put the card into an
  undefined state.
- Use 10 kΩ, as the SD spec and IDF recommend. The internal pull-ups
  (~45 kΩ) are too weak. We measured the same effect on I2C: internal
  pull-ups gave 429–508 ns rise times, too slow for 400 kHz, and SD runs at
  20–40 MHz. Internal pull-ups are only good enough for a quick bench test
  at 400 kHz.
- Put a 22–33 Ω series resistor near the ESP32-S3 on CLK when traces are
  long. This damps ringing at high clock rates.

## Pin choice on the S3

- The S3's SDMMC host routes through the GPIO matrix, so any free GPIO
  works. The ESP32 (non-S3) problem with GPIO12/MTDI and a DAT2 pull-up
  does **not** exist on the S3.
- Keep the SD lines off the four S3 strapping pins (TRM ch. 8 "Strapping
  Pins"), because a 10 kΩ pull-up there changes how the chip boots:
  - **GPIO45** selects VDD_SPI at reset. A pull-up selects the 1.8 V LDO,
    and a 3.3 V flash then fails to boot. Never put an SD pull-up on GPIO45.
  - **GPIO0**: a pull-up is harmless (it is the normal SPI-boot level), but
    the pin is the BOOT button on most boards.
  - **GPIO46**: boot mode / ROM log. It must be low or floating for
    download mode, so no pull-up.
  - **GPIO3**: selects the JTAG source in some eFuse configurations.
- Avoid GPIO26–32 (in-package flash) and, on octal-PSRAM modules,
  GPIO33–37.
- GPIO19 and GPIO20 are the USB console used by esp-ndf.

## Card detect / write protect

Both are optional. If used, they are plain inputs with their own pull-up
(internal is fine, since they change slowly). esp-ndf reads them with
`gpio_read`.
