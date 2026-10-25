# esp-ndf threat model

## Assets
Firmware integrity (what runs), keys and secrets in RAM / NVS (NVS encryption key, OTA public key, application keys),
data on flash filesystems, correct actuator control (robots: a wrong output is a physical hazard), availability.

## Attackers and entry points
| Attacker | Reaches | Defences in the SDK |
|---|---|---|
| USB host (cable) | USB control requests, MSC SCSI commands, CDC/HID data | bounded descriptors, overflow-safe LBA checks + bdev range checks (fuzz_msc), class requests per interface |
| Removable / host-written media | FAT volumes (SD card, the MSC disk) | LFN/sequence checks, bounded names, cluster-chain loops bounded (fuzz_fat) |
| Corrupted or tampered flash | partition table, NVS, SPIFFS, PFS, WL state, otadata, OTA images, core dump | partition entry validation + required MD5, CRC + sanity checks on every structure, fuzzers for each, signed OTA (ECDSA P-256) with version floor, loader checks image + segment ranges + SHA-256 |
| Radio (BLE, 802.11, 802.15.4) | received frames and the controller's descriptors | lengths clamped to buffers, descriptor offsets checked against the buffer |
| External devices on buses | I2C/SPI/UART/TWAI/camera/touch data | counts and lengths capped (GT911 points, DMA lengths), bounded waits on every bus |
| Untrusted data in the app | NN models, printf formats, caller arguments | model parameter checks, %n off, index checks on every driver API |
| Physical attacker with JTAG / flash access | everything | NOT mitigated in this SDK: secure boot, flash encryption and JTAG disable need eFuses (owner decision: no eFuse burns). Accepted risk; a product must enable them. |
| Fault injection (glitch) | crypto operations | ECDSA double computation of k G (negated scalar) + s k = e + r d check, hedged nonces; glitch / brownout resets on |
| Timing side channels | RSA, ECC, MAC compare | branch-free P-256 field and point code with full table scans, constant-time tag compares; RSA fast path leaks only a fixed key's length / weight (constant-time engine mode optional) |

## Trust assumptions
All code in the image is trusted (no privilege separation between tasks: bare metal, one world). The running image
is authentic only as far as the OTA signature and the eFuse-based protections (not enabled) guarantee.

## Out of scope here
Wireless protocol stacks (added later, audited then), application logic.
