/* Host test + fuzzer of components/wifi/{wpa_sha1.c,eapol.c}: the generated vectors (PMK, HMAC-SHA1, PTK, EAPOL-Key MIC), then random and mutated
 * EAPOL-Key frames with truncated or inflated length fields. usage: fuzz_eapol [rounds] [seed] */
#include "wpa.h"
#include "vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long x = 1;
static unsigned rnd(void) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return (unsigned)x; }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv)
{
    unsigned long rounds = argc > 1 ? strtoul(argv[1], 0, 0) : 200000; x = argc > 2 ? strtoul(argv[2], 0, 0) | 1 : 7;
    uint8_t o[48], h[20];
    wpa_pmk((const char *)pmk_pass0, pmk_ssid0, sizeof pmk_ssid0, o); CHECK(!memcmp(o, pmk_out0, 32));
    wpa_pmk((const char *)pmk_pass1, pmk_ssid1, sizeof pmk_ssid1, o); CHECK(!memcmp(o, pmk_out1, 32));
    wpa_pmk((const char *)pmk_pass2, pmk_ssid2, sizeof pmk_ssid2, o); CHECK(!memcmp(o, pmk_out2, 32));
    wpa_hmac_sha1(hm_key0, sizeof hm_key0, hm_dat0, sizeof hm_dat0, h); CHECK(!memcmp(h, hm_out0, 20));
    wpa_hmac_sha1(hm_key1, sizeof hm_key1, hm_dat1, sizeof hm_dat1, h); CHECK(!memcmp(h, hm_out1, 20));
    wpa_ptk(ptk_pmk, ptk_aa, ptk_spa, ptk_an, ptk_sn, o); CHECK(!memcmp(o, ptk_out, 48));
    CHECK(eapol_key_check(eapol_kck, eapol_frame, sizeof eapol_frame) == 0);
    eapol_key_t k; CHECK(eapol_key_parse(eapol_frame, sizeof eapol_frame, &k) == 0 && k.replay == 7 && k.data_len == 24 && !memcmp(k.nonce, ptk_an, 32));
    for (unsigned long i = 0; i < rounds && !fails; i++) {
        unsigned len = rnd() % (sizeof eapol_frame + 40); uint8_t *f = malloc(len ? len : 1);
        for (unsigned j = 0; j < len; j++) f[j] = j < sizeof eapol_frame && (rnd() & 7) ? eapol_frame[j] : (uint8_t)rnd();    /* mostly the valid frame, damaged */
        if (len > 3 && (rnd() & 1)) { f[2] = (uint8_t)(rnd() & 1 ? 0 : rnd()); f[3] = (uint8_t)rnd(); }                         /* length field lies */
        if (len > 98 && (rnd() & 1)) { f[97] = (uint8_t)rnd(); f[98] = (uint8_t)rnd(); }                                         /* key data length lies */
        if (!eapol_key_parse(f, len, &k)) { CHECK(k.data + k.data_len <= f + len); CHECK(k.nonce + 32 <= f + len && k.mic + 16 <= f + len); }
        if (!eapol_key_check(eapol_kck, f, len)) CHECK(len == sizeof eapol_frame || !memcmp(f, eapol_frame, sizeof eapol_frame));     /* a valid MIC only on the real frame */
        free(f);
    }
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("fuzz_eapol: ok (%lu rounds)\n", rounds);
    return 0;
}
