/* EAPOL-Key frame parsing and MIC check: pure, every length checked against the frame. Layout (802.11i 8.5.2):
 * 802.1X header 4 | descriptor type 1 | key info 2 | key length 2 | replay counter 8 | nonce 32 | IV 16 | RSC 8 | ID 8 | MIC 16 | data length 2 | data */
#include <string.h>
#include "wpa.h"

#define KEY_BODY 95                                     /* bytes from the descriptor type to the end of the data length field */

int eapol_key_parse(const uint8_t *f, unsigned n, eapol_key_t *k)
{
    if (n < 4 + KEY_BODY || f[1] != 3) return -1;                                     /* EAPOL-Key packet type */
    unsigned body = (unsigned)(f[2] << 8 | f[3]);
    if (body < KEY_BODY || 4 + body > n || f[4] != 2) return -1;                      /* RSN descriptor */
    k->info = (uint16_t)(f[5] << 8 | f[6]); k->key_len = (uint16_t)(f[7] << 8 | f[8]);
    k->replay = 0; for (int i = 0; i < 8; i++) k->replay = k->replay << 8 | f[9 + i];
    k->nonce = f + 17; k->mic = f + 81; k->data_len = (uint16_t)(f[97] << 8 | f[98]); k->data = f + 99;
    if (99u + k->data_len > 4 + body) return -1;                                      /* key data past the frame */
    return 0;
}

int eapol_key_check(const uint8_t kck[16], const uint8_t *frame, unsigned n)
{
    eapol_key_t k; uint8_t cp[512], out[20];
    if (eapol_key_parse(frame, n, &k) || n > sizeof cp) return -1;
    unsigned len = 4 + (unsigned)(frame[2] << 8 | frame[3]);                          /* the 802.1X body only, not trailing bytes */
    memcpy(cp, frame, len); memset(cp + 81, 0, 16);
    wpa_hmac_sha1(kck, 16, cp, len, out);
    uint8_t d = 0; for (int i = 0; i < 16; i++) d |= out[i] ^ frame[81 + i];
    return d ? -1 : 0;
}
