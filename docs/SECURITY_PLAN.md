# E5 / E8 security plan (needs approval: every step below that burns an eFuse is permanent)

State read with `espefuse.py summary` (read-only) on 2026-10-24: on both boards (C9:D0, BOX-3 C1:10) all six key
blocks BLOCK_KEY0..5 are empty with purpose USER, flash encryption is off (SPI_BOOT_CRYPT_CNT = 0), secure boot is
off, download mode is enabled. Nothing has been burned for E5/E8.

## E5 - HMAC and Digital Signature (DS)

What it needs: a 256-bit key in one key block with purpose HMAC_UP (HMAC for software use), HMAC_DOWN_JTAG
(re-enable JTAG) or HMAC_DOWN_DIGITAL_SIGNATURE (DS key derivation). Once burned, the key is unreadable by software
and the block and its purpose are fixed for the life of the chip. The rest of the chip is unaffected: other key
blocks, flash, boot and JTAG keep working (unless the JTAG soft-disable fuse is also burned - not planned).

Proposed steps (one board, BOX-3 or a spare, never both until the first passes):
1. Write drivers first (no burn): drivers/crypto/hmac.c (upstream mode: key block select, message by SHA-style
   padding, 32-byte result; downstream mode for JTAG and DS) and drivers/crypto/ds.c (load the encrypted RSA
   parameters, start, read the signature), following esp_hw_support/esp_hmac.c and esp_ds.c. Build-checked only.
2. Burn ONE test key: `espefuse.py burn_key BLOCK_KEY5 test_hmac_key.bin HMAC_UP` (key from a file kept in
   bench/hmac/, known value so results can be checked against Python's hmac.new(key, msg, sha256)).
   Irreversible for BLOCK_KEY5 only.
3. Test HMAC upstream vs Python on 0..1000-byte messages; time vs IDF esp_hmac_calculate on the same board.
4. DS needs a second key (HMAC_DOWN_DIGITAL_SIGNATURE) in another block (e.g. BLOCK_KEY4), plus the RSA key
   parameters encrypted with it (esp_ds tooling or our own host script); then sign vs Python RSA verify.

Approval needed for: step 2 (BLOCK_KEY5, HMAC_UP) and, separately, step 4 (BLOCK_KEY4, HMAC_DOWN_DS).

## E8 - flash / PSRAM encryption and secure boot v2

Both are one-way for the whole chip and change how every later image must be built and flashed:
- Flash encryption: burns an XTS-AES key (BLOCK_KEYn, purpose XTS_AES_128_KEY or 256_KEY_1/2) and increments
  SPI_BOOT_CRYPT_CNT. In "development" mode it can be toggled off/on only 3 times in the chip's life; "release"
  mode also write-protects it. Our loader, OTA, NVS-encryption and flash driver would have to handle encrypted
  writes (the flash driver gains an encrypted-write path through the XTS engine).
- Secure boot v2: burns up to 3 RSA-PSS public-key digests and SECURE_BOOT_EN. From then on the ROM only runs
  images signed with one of those keys - our loader would have to be signed and verify the app. A lost signing key
  bricks the board for new firmware.

Recommendation: do E8 only on a dedicated spare board, in this order: (1) implement and test the encrypted-flash
write path and image signing against the TRM with no fuses burned (the XTS engine can be exercised in software
mode via our aes_xts), (2) flash encryption in development mode on the spare, (3) secure boot last. Each burn is a
separate approval.
