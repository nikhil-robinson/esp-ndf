/* WPA2-PSK primitives (IEEE 802.11i): PMK, PTK, EAPOL-Key MIC, AES key unwrap, CCMP. wpa_sha1.c and eapol.c are pure software (host tests);
 * wpa_ccmp.c uses the AES engine through aes_block(). Secrets are wiped with explicit_bzero. */
#pragma once
#include <stdint.h>

void wpa_hmac_sha1(const uint8_t *key, unsigned klen, const uint8_t *data, unsigned n, uint8_t out[20]);
void wpa_pmk(const char *pass, const uint8_t *ssid, unsigned ssid_len, uint8_t pmk[32]);        /* PBKDF2-HMAC-SHA1, 4096 iterations */
void wpa_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]);   /* PRF-384: KCK 0..15, KEK 16..31, TK 32..47 */
#define WPA_UNWRAP_MAX 192                                                                     /* key data of message 3: RSN element + GTK + IGTK KDEs fit */
int  aes_unwrap(const uint8_t kek[16], const uint8_t *in, unsigned n, uint8_t *out);            /* RFC 3394; n = 8 + key length (<= WPA_UNWRAP_MAX); out holds n - 8; returns the key length or -1 */
int  aes_wrap(const uint8_t kek[16], const uint8_t *in, unsigned n, uint8_t *out);               /* RFC 3394; n = key length; writes n + 8 bytes, returns that length or -1 */

/* SHA-256 / CMAC key hierarchy (PMF, WPA3; wpa_sha256.c) */
void wpa_hmac_sha256(const uint8_t *key, unsigned kl, const uint8_t *d, unsigned n, uint8_t out[32]);
void wpa_hmac_sha256_v(const uint8_t *key, unsigned kl, unsigned cnt, const uint8_t *const *p, const unsigned *l, uint8_t out[32]);
void wpa_kdf_sha256(const uint8_t *key, unsigned kl, const char *label, const uint8_t *ctx, unsigned cl, uint8_t *out, unsigned bits);   /* 802.11 KDF-SHA256, bits a multiple of 8 */
void wpa_ptk_sha256(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]);   /* AKM 6 / 8 */
void wpa_cmac(const uint8_t key[16], const uint8_t *d, unsigned n, uint8_t out[16]);
void wpa_cmac_v(const uint8_t key[16], unsigned cnt, const uint8_t *const *p, const unsigned *l, uint8_t out[16]);
int  wpa_bip_check(const uint8_t igtk[16], const uint8_t *f, unsigned n, uint64_t *ipn);           /* BIP-CMAC-128 over a frame ending in the MMIE; 0 good */

/* CCMP. body points at the 8-byte CCMP header; the data (n bytes) follows it and the 8-byte MIC goes behind the data. hdr = the 802.11 header (hl bytes:
 * 24, 26 with QoS, +6 with four addresses). */
int  ccmp_encrypt(const uint8_t tk[16], const uint8_t *hdr, unsigned hl, uint64_t pn, uint8_t key_id, uint8_t *body, unsigned n);   /* returns n + 16 */
int  ccmp_decrypt(const uint8_t tk[16], const uint8_t *hdr, unsigned hl, uint8_t *body, unsigned total, uint64_t *pn_out);          /* data length (in place at body + 8), -1 = bad MIC / malformed */

/* EAPOL-Key (802.1X, descriptor type 2). Offsets inside the EAPOL frame. */
typedef struct { uint16_t info, key_len, data_len; uint64_t replay; const uint8_t *nonce, *rsc /* key RSC, 8 bytes LE */, *mic, *data; } eapol_key_t;
#define EAPOL_INFO_MIC     0x0100
#define EAPOL_INFO_PAIRWISE 0x0008
#define EAPOL_INFO_ACK     0x0080
#define EAPOL_INFO_INSTALL 0x0040
#define EAPOL_INFO_SECURE  0x0200
#define EAPOL_INFO_ENCDATA 0x1000
int  eapol_key_parse(const uint8_t *f, unsigned n, eapol_key_t *k);                              /* 0 ok, -1 malformed (lengths past the frame, wrong type) */
int  eapol_key_check(const uint8_t kck[16], const uint8_t *frame, unsigned n);                  /* HMAC-SHA1-128 MIC over the frame with the MIC field zeroed, constant time; 0 ok */
int  eapol_key_check_cmac(const uint8_t kck[16], const uint8_t *frame, unsigned n);             /* AES-128-CMAC MIC (descriptor version 3 / AKM-defined 0) */
