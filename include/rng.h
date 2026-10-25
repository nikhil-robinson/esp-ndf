/* Random numbers. rng_hw: true random from the hardware RNG (entropy from the 8 MHz oscillator, plus RF/ADC noise when
 * those run), paced to the rate ESP-IDF validated (~45 kHz words). rng_fill / rng_u32: ChaCha20 generator seeded with
 * 256 hardware bits, re-keyed after every call (fast key erasure) and reseeded from hardware every NDF_RNG_RESEED
 * bytes: cryptographic quality at memory speed. Safe from both cores and interrupts. */
#pragma once
#include "ndf.h"

void     rng_init(void);                    /* RNG clock + entropy source on; called by the first use too */
void     rng_boot_seed(void);               /* at boot, before main: seed with the SAR ADC noise source on (IDF bootloader_random) */
uint32_t rng_hw(void);                      /* one true-random word (~22 us) */
void     rng_fill(void *buf, size_t len);   /* fast cryptographic random bytes */
uint32_t rng_u32(void);
int      rng_selftest(void);                /* ChaCha20 known answer (RFC 8439): 0 = pass */
