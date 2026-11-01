/* LE Secure Connections pairing (Just Works) and the crypto toolbox of the Security Manager (Core spec vol 3 part H). Byte arrays are as they appear on the air, least significant
 * octet first; the functions turn them around internally (the specification writes them most significant first). Pure code apart from AES (aes_block): the host test
 * tools/fuzz/test_smp.c checks every function against the specification's sample data. */
#pragma once
#include <stdint.h>

void smp_cmac(const uint8_t key[16], const uint8_t *msg, unsigned n, uint8_t out[16]);                     /* AES-CMAC, RFC 4493, byte strings as given (most significant first) */
void smp_f4(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], uint8_t z, uint8_t out[16]);     /* confirm value */
void smp_f5(const uint8_t w[32], const uint8_t n1[16], const uint8_t n2[16], const uint8_t a1[7], const uint8_t a2[7], uint8_t mackey[16], uint8_t ltk[16]);   /* a: address type, then address (LSO first) */
void smp_f6(const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16], const uint8_t r[16], const uint8_t io[3], const uint8_t a1[7], const uint8_t a2[7], uint8_t out[16]);   /* check value */
uint32_t smp_g2(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], const uint8_t y[16]);       /* numeric comparison value (before mod 10^6) */
void smp_ah(const uint8_t irk[16], const uint8_t r[3], uint8_t hash[3]);                                    /* resolvable private address hash */
void smp_e(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);                                   /* AES-128 with LSO-first arrays: the "e" of the specification */

/* ---- pairing (peripheral / responder): LE Secure Connections, Just Works, bonding with stored keys ----
 * smp_rx takes the L2CAP payload of CID 6; answers go out through the send callback. The link layer asks for the long term key with smp_ltk (LL_ENC_REQ) and tells
 * smp_encrypted when encryption started; then the identity keys are exchanged and the bond is stored. Storage is one blob (identity resolving key + bonds) through the
 * hooks, so the same code runs on the host test, from RAM only, or over NVS. */
typedef struct { uint8_t used, has_irk, key_size, addr[7]; uint8_t ltk[16], irk[16]; } smp_bond_t;      /* addr: type, then 6 octets LSO first (identity address once known) */
#define SMP_BONDS 4
void smp_init(int (*send)(const uint8_t *pdu, unsigned n));
void smp_set_store(int (*load)(void *blob, unsigned *n), int (*save)(const void *blob, unsigned n));       /* load: n = buffer size in, size out; 0 = ok */
void smp_connected(const uint8_t peer[7], const uint8_t own[7]);                                          /* a link starts: both addresses as type + 6 octets */
void smp_disconnected(void);
void smp_rx(const uint8_t *pdu, unsigned n);
int  smp_ltk(const uint8_t rand[8], uint16_t ediv, uint8_t ltk[16]);                                       /* 0 = key found for this link (LL_ENC_REQ), else reject */
void smp_encrypted(int on);                                                                               /* the link layer started (1) / lost (0) encryption */
unsigned smp_bonds(smp_bond_t *out, unsigned max);                                                        /* stored bonds (testing) */
void smp_bonds_clear(void);
int  smp_is_encrypted(void);
void smp_local_irk(uint8_t irk[16]);                                                                      /* our identity resolving key (created on first use) */
