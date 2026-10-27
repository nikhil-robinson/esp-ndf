#include "unit.h"
#include "console.h"
#include "sys.h"
#include <string.h>
#include "wpa.h"
#include "vectors.h"

#define SAME(a, b) (sizeof(a) == sizeof(b) && !memcmp(a, b, sizeof(a)))

TEST(pmk)                                               /* IEEE 802.11i annex H.4 and a 63-character passphrase */
{
    static const uint8_t *pw[] = { pmk_pass0, pmk_pass1, pmk_pass2 }, *ss[] = { pmk_ssid0, pmk_ssid1, pmk_ssid2 }, *ex[] = { pmk_out0, pmk_out1, pmk_out2 };
    static const unsigned sl[] = { sizeof pmk_ssid0, sizeof pmk_ssid1, sizeof pmk_ssid2 };
    for (int i = 0; i < N_PMK; i++) { uint8_t o[32]; wpa_pmk((const char *)pw[i], ss[i], sl[i], o); UASSERT(!memcmp(o, ex[i], 32)); }
    uint8_t o[32]; uint32_t t = time_ms(); wpa_pmk("password", (const uint8_t *)"IEEE", 4, o);
    printf("      PBKDF2 4096 iterations: %lu ms\n", (unsigned long)(time_ms() - t));
    UASSERT(time_ms() - t < 150);
}
TEST(hmac_sha1)
{
    uint8_t o[20];
    wpa_hmac_sha1(hm_key0, sizeof hm_key0, hm_dat0, sizeof hm_dat0, o); UASSERT(!memcmp(o, hm_out0, 20));
    wpa_hmac_sha1(hm_key1, sizeof hm_key1, hm_dat1, sizeof hm_dat1, o); UASSERT(!memcmp(o, hm_out1, 20));
}
TEST(ptk)
{
    uint8_t o[48]; wpa_ptk(ptk_pmk, ptk_aa, ptk_spa, ptk_an, ptk_sn, o); UASSERT(!memcmp(o, ptk_out, 48));
    wpa_ptk(ptk_pmk, ptk_spa, ptk_aa, ptk_sn, ptk_an, o); UASSERT(!memcmp(o, ptk_out, 48));       /* the order of the two sides does not matter */
}
TEST(eapol_mic)
{
    UASSERT(eapol_key_check(eapol_kck, eapol_frame, sizeof eapol_frame) == 0);
    uint8_t f[sizeof eapol_frame]; memcpy(f, eapol_frame, sizeof f);
    f[90] ^= 1; UASSERT(eapol_key_check(eapol_kck, f, sizeof f) != 0);                                 /* one MIC bit */
    memcpy(f, eapol_frame, sizeof f); f[20] ^= 1; UASSERT(eapol_key_check(eapol_kck, f, sizeof f) != 0);   /* one nonce bit */
    UASSERT(eapol_key_check(eapol_kck, eapol_frame, sizeof eapol_frame - 1) != 0);                     /* truncated */
}
TEST(unwrap)                                            /* RFC 3394 with a 128-bit KEK */
{
    uint8_t o[64];
    UASSERT(aes_unwrap(uw_kek, uw_in0, sizeof uw_in0, o) == 16 && !memcmp(o, uw_key0, 16));
    UASSERT(aes_unwrap(uw_kek, uw_in1, sizeof uw_in1, o) == 32 && !memcmp(o, uw_key1, 32));
    uint8_t bad[sizeof uw_in0]; memcpy(bad, uw_in0, sizeof bad); bad[3] ^= 1;
    UASSERT(aes_unwrap(uw_kek, bad, sizeof bad, o) == -1);
}
static void one_ccmp(const uint8_t *hdr, unsigned hl, const uint8_t *pt, unsigned n, const uint8_t *ct, unsigned cl, uint64_t pn, int *ok)
{
    static uint8_t b[1600]; uint64_t pn2 = 0;
    memcpy(b + 8, pt, n);
    *ok &= ccmp_encrypt(ccmp_tk, hdr, hl, pn, 0, b, n) == (int)n + 16 && cl == n + 16 && !memcmp(b, ct, cl);
    memcpy(b, ct, cl);
    *ok &= ccmp_decrypt(ccmp_tk, hdr, hl, b, cl, &pn2) == (int)n && pn2 == pn && !memcmp(b + 8, pt, n);
    if (cl > 9) { memcpy(b, ct, cl); b[cl - 1] ^= 1; *ok &= ccmp_decrypt(ccmp_tk, hdr, hl, b, cl, 0) == -1; }     /* MIC bit */
    memcpy(b, ct, cl); ((uint8_t *)b)[8] ^= 1; *ok &= (n ? ccmp_decrypt(ccmp_tk, hdr, hl, b, cl, 0) == -1 : 1);       /* data bit */
}
TEST(ccmp)
{
    int ok = 1;
#define C(i) one_ccmp(cc_hdr##i, sizeof cc_hdr##i, cc_pt##i, sizeof cc_ct##i - 16, cc_ct##i, sizeof cc_ct##i, cc_pn##i, &ok)
    C(0); C(1); C(2); C(3); C(4); C(5); C(6); C(7);
    UASSERT(ok);
}
