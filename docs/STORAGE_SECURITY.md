# Storage security in esp-ndf

What is protected today, what is not, and what enabling the remaining
pieces involves. Some of those steps burn eFuses, which is irreversible.
Each such step is marked, and none of them is done by esp-ndf code or tests.

## Status

| Feature | Status | Notes |
|---|---|---|
| NVS encryption (XTS-AES-256, IDF scheme) | **done, verified** | `nvs_mount_secure()`; on-flash format identical to IDF (both directions + host tool) |
| Keys partition (`nvs_keys`: eky, tky, crc32) | **done** | `nvs_read_keys()`; `tools/nvstool.py keygen` |
| Encrypted NVS images on the host | **done** | `nvstool.py gen --keyfile`, byte-identical to IDF |
| Per-device encrypted images | **done** | `mfgtool.py --keygen` (one key file per device) |
| HMAC key protection (key never leaves eFuse) | key derivation only | host: `keygen --hmac-key`. Device side needs an HMAC key in eFuse **(irreversible)** |
| Flash encryption | not enabled | **irreversible** eFuse burn |
| Secure boot | not enabled | **irreversible** eFuse burn |
| FAT / SPIFFS contents | plaintext | no IDF equivalent either, apart from flash encryption |

## Threat model, plainly

- **NVS encryption alone protects against reading a dumped NVS
  partition.** It does not protect against someone who can also dump the
  `nvs_keys` partition: without flash encryption, that partition is
  plaintext on the same chip.
- The key has to be somewhere the attacker can't read. There are two ways,
  and IDF uses the same two:
  1. **Flash encryption.** The ROM loader decrypts the image, and
     `nvs_keys` is stored encrypted. The keys are protected by a key in
     eFuse that software cannot read.
  2. **HMAC scheme.** The 64-byte NVS key is derived at boot as
     HMAC-SHA256(eFuse key, fixed seeds), computed inside the HMAC
     peripheral. Nothing secret is stored in flash.
- Wrong or missing keys are handled safely, which the board tests verify:
  - a wrong key reads nothing, because every entry fails its CRC;
  - nothing is written by a read-only probe;
  - plaintext never appears in the encrypted partition.

## Recommended path for a product

1. Development (now): `nvs_keys` partition flashed from
   `nvstool.py keygen` and per-device images from `mfgtool.py`.
2. Production, pick one:
   - **HMAC scheme.** Burn one HMAC key per device (`espefuse.py
     burn_key BLOCK_KEYx key.bin HMAC_UP`, **irreversible**) and derive
     the NVS key on the device. This needs the HMAC peripheral driver,
     which is not written yet. The host derivation in `nvstool.py keygen
     --hmac-key` produces the same 64 bytes, so production images can be
     generated offline.
   - **Flash encryption.** The ROM loads esp-ndf's RAM image from
     flash 0x0, and the ROM loader supports encrypted images. Enabling it
     burns the flash-encryption key and `SPI_BOOT_CRYPT_CNT`
     (**irreversible**; "release" mode also disables re-flashing in
     plaintext).
3. Secure boot (signed images) is independent of 2. and also
   **irreversible**.

Before any eFuse step, test on a spare board. The two test boards on this
bench have not been modified, and no esp-ndf test burns eFuses.

## Data at rest outside NVS

FAT and SPIFFS store data in plaintext, as on IDF without flash encryption.
To protect calibration or logs, store them as NVS blobs (encrypted) or
enable flash encryption.
