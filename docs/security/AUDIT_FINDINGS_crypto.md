# Crypto & secrets findings (esp-ndf, 2026-10-25) - ALL FIXED (commit 7c51a3f, pass 2 in 2805ec0)

| ID | Sev | Location | Issue | Fix |
|---|---|---|---|---|
| K1 | HIGH | drivers/crypto/rng.c:22-28 | rng_init enables only the RC 8 MHz source; IDF bootloader_random_enable also turns on the SAR ADC noise source. With RF off the hardware RNG is not a full-entropy source, and keys / GCM IVs come from it | enable the SAR ADC entropy path as IDF does (or require RF on) before seeding; health test on the raw words |
| K2 | MEDIUM | soc/esp32s3/coredump.c | panic dump stores raw stack (may hold keys) in plaintext flash | see M3 |
| K3 | MEDIUM | lib/heap.c free | freed blocks are not cleared; secrets in heap survive | secure-free helper / clear key buffers in crypto APIs |
| K4 | LOW | drivers/crypto/*, lib/ecc.c, lib/bigint.c | no fault-injection countermeasures (double computation / verify after sign) | optional verify-after-sign |

Checked OK: GCM tag compare is constant time (gcm.c:74); ECC scalar multiplication documented branch-free on secret
data (ecc.c:130, still to be confirmed by a timing test); ChaCha20 DRBG with fast key erasure (rng.c); no hardcoded
credentials in SDK sources.
Not yet verified: RSA modexp timing behaviour; key clearing after use in aes/rsa/ecc.
