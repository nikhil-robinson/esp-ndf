# esp-ndf audit map (2026-10-25, branch ndf-core)

## Code
- C11 + Xtensa assembly, bare metal, CALL0 ABI, GCC 13.2 (esp-13.2.0_20250707). ~35k lines: arch/xtensa (1k),
  soc/esp32s3 (2.3k), drivers (16.3k), lib (13.5k, of which lib/musl is vendored libm/printf helpers), include (2.3k).
- No RTOS: one main loop per core, cooperative threads (lib/coop.c), interrupts (arch/xtensa/irq.c), spinlocks.
- Examples (examples/*) and host tools (tools/*) are not part of the SDK image but ship in the repo.

## Entry points where untrusted data comes in (trust boundaries)
| Boundary | Code | Who controls the data |
|---|---|---|
| USB device control requests | drivers/usb/usb_dev.c handle_setup | USB host |
| USB mass storage CBW / SCSI | drivers/usb/usb_msc.c command | USB host |
| USB CDC / HID data | usb_dev.c usb_pipe_read | USB host |
| Console (USB-JTAG / UART0) line input + shell | drivers/console/usj.c, lib/cmd.c | whoever has the cable |
| UART / I2C / SPI / CAN (TWAI) / I2S / RMT / camera input | drivers/* | external devices |
| FAT volume (SD card, USB MSC disk, flash partition) | drivers/storage/fat.c | removable media, USB host |
| SPIFFS / PFS / NVS / WL / partition table / OTA data / core dump in flash | drivers/storage/* | flash contents (corruption, physical access) |
| OTA image | drivers/storage/ota.c | update source |
| Radio receive (BLE, 802.11, 802.15.4, raw IQ) | drivers/ble/*, drivers/rf/* | anyone in radio range |
| NN models, DSP inputs | lib/nn.c, lib/dsp_* | application data / model files |
| JTAG | hardware (USB-JTAG, OpenOCD), drivers/trace | physical access |

## Secrets and keys
- NVS encryption keys: nvs_keys partition, read by nvs_read_keys (drivers/storage/nvs.c:512), kept in RAM.
- AES / SHA / RSA / ECC / GCM keys: caller memory (drivers/crypto/*, lib/ecc.c, lib/bigint.c); AES key registers.
- RNG state: drivers/crypto/rng.c (ChaCha20 key in RAM).
- No hardcoded credentials found in SDK sources (grep: key/secret/password/psk).
- eFuse keys (HMAC/DS/flash encryption): not used - excluded by the owner (no eFuse burns).
