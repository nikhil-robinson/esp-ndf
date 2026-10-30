/* WPA3-SAE (IEEE 802.11-2020 12.4), group 19 (P-256), hunting-and-pecking PWE as IDF's supplicant does by default (sae.c in its
 * wpa_supplicant). Pure apart from p256 / sha / rng: the host tests and the fuzzer drive it. */
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t pwe[64], rand[32], scalar[32], elem[64];          /* own: PWE, rand, commit scalar and element */
    uint8_t peer_scalar[32], peer_elem[64];
    uint8_t kck[32], pmk[32], pmkid[16];
    uint16_t send_confirm;
} sae_t;

int      sae_prepare(sae_t *s, const uint8_t me[6], const uint8_t peer[6], const char *pw, unsigned pl);     /* PWE + commit with fresh rand / mask; 0 ok */
int      sae_prepare_fixed(sae_t *s, const uint8_t me[6], const uint8_t peer[6], const char *pw, unsigned pl, const uint8_t rand[32], const uint8_t mask[32]);   /* test vectors */
unsigned sae_write_commit(const sae_t *s, uint8_t *o, const uint8_t *token, unsigned tl);   /* group | anti-clogging token | scalar | element */
int      sae_process_commit(sae_t *s, const uint8_t *b, unsigned n);    /* peer's commit body (group | scalar | element): checked, then K, KCK, PMK, PMKID; 0 ok */
unsigned sae_write_confirm(sae_t *s, uint8_t *o);                       /* send-confirm | confirm */
int      sae_check_confirm(const sae_t *s, const uint8_t *b, unsigned n);   /* peer's confirm body; 0 = the peer knows the password */
void     sae_wipe(sae_t *s);
