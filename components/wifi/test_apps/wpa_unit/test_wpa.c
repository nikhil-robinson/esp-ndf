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
TEST(wrap)                                              /* the same RFC 3394 vectors, forwards */
{
    uint8_t o[64];
    UASSERT(aes_wrap(uw_kek, uw_key0, sizeof uw_key0, o) == (int)sizeof uw_in0 && !memcmp(o, uw_in0, sizeof uw_in0));
    UASSERT(aes_wrap(uw_kek, uw_key1, sizeof uw_key1, o) == (int)sizeof uw_in1 && !memcmp(o, uw_in1, sizeof uw_in1));
    UASSERT(aes_wrap(uw_kek, uw_key0, 12, o) == -1 && aes_wrap(uw_kek, uw_key0, 8, o) == -1);   /* not a multiple of 8 / too short */
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
    C(0); C(1); C(2); C(3); C(4); C(5); C(6); C(7); C(8);    /* 8: a protected management frame */
    UASSERT(ok);
}
TEST(cmac)                                              /* RFC 4493 examples 1-4 */
{
    uint8_t o[16];
    wpa_cmac(cmac_key, cmac_msg, CMAC_LEN0, o); UASSERT(!memcmp(o, cmac_out0, 16));
    wpa_cmac(cmac_key, cmac_msg, CMAC_LEN1, o); UASSERT(!memcmp(o, cmac_out1, 16));
    wpa_cmac(cmac_key, cmac_msg, CMAC_LEN2, o); UASSERT(!memcmp(o, cmac_out2, 16));
    wpa_cmac(cmac_key, cmac_msg, CMAC_LEN3, o); UASSERT(!memcmp(o, cmac_out3, 16));
    const uint8_t *p[3] = { cmac_msg, cmac_msg + 7, cmac_msg + 23 }; unsigned l[3] = { 7, 16, 17 };   /* the same 40 bytes split unevenly */
    wpa_cmac_v(cmac_key, 3, p, l, o); UASSERT(!memcmp(o, cmac_out2, 16));
}
TEST(ptk_sha256)                                        /* KDF-SHA256 PTK (AKM 6 / 8) */
{
    uint8_t o[48]; wpa_ptk_sha256(ptk_pmk, ptk_aa, ptk_spa, ptk_an, ptk_sn, o); UASSERT(!memcmp(o, ptk256_out, 48));
    wpa_ptk_sha256(ptk_pmk, ptk_spa, ptk_aa, ptk_sn, ptk_an, o); UASSERT(!memcmp(o, ptk256_out, 48));
}
TEST(eapol_mic_cmac)
{
    UASSERT(eapol_key_check_cmac(eapol_kck2, eapol_frame2, sizeof eapol_frame2) == 0);
    uint8_t f[sizeof eapol_frame2]; memcpy(f, eapol_frame2, sizeof f);
    f[90] ^= 1; UASSERT(eapol_key_check_cmac(eapol_kck2, f, sizeof f) != 0);
    UASSERT(eapol_key_check_cmac(eapol_kck, eapol_frame, sizeof eapol_frame) != 0);                    /* an HMAC-SHA1 frame does not pass as CMAC */
}
TEST(bip)                                               /* BIP-CMAC-128 on a broadcast deauthentication */
{
    uint64_t ipn = 0; UASSERT(wpa_bip_check(bip_igtk, bip_frame, sizeof bip_frame, &ipn) == 0 && ipn == bip_ipn);
    uint8_t f[sizeof bip_frame]; memcpy(f, bip_frame, sizeof f); f[24] ^= 1; UASSERT(wpa_bip_check(bip_igtk, f, sizeof f, &ipn) != 0);   /* reason code */
    memcpy(f, bip_frame, sizeof f); f[1] |= 0x08; UASSERT(wpa_bip_check(bip_igtk, f, sizeof f, &ipn) == 0);   /* the retry bit is not covered */
    UASSERT(wpa_bip_check(bip_igtk, bip_frame, sizeof bip_frame - 1, &ipn) != 0);
}
#include "sae.h"
#include "sae_vectors.h"
TEST(sae_j10)                                           /* 802.11-2020 Annex J.10: PWE (through the commit element), commit, KCK, PMK, PMKID */
{
    static sae_t s; uint8_t o[128];
    uint32_t t = time_ms();
    UASSERT(sae_prepare_fixed(&s, sae_addr1, sae_addr2, "mekmitasdigoat", 14, sae_local_rand, sae_local_mask) == 0);
    printf("      SAE PWE (40 rounds) + commit: %lu ms\n", (unsigned long)(time_ms() - t));
    UASSERT(sae_write_commit(&s, o, 0, 0) == sizeof sae_local_commit && !memcmp(o, sae_local_commit, sizeof sae_local_commit));
    t = time_ms();
    UASSERT(sae_process_commit(&s, sae_peer_commit, sizeof sae_peer_commit) == 0);
    printf("      SAE K + keys: %lu ms\n", (unsigned long)(time_ms() - t));
    UASSERT(!memcmp(s.kck, sae_kck, 32) && !memcmp(s.pmk, sae_pmk, 32) && !memcmp(s.pmkid, sae_pmkid, 16));
    uint8_t bad[sizeof sae_peer_commit]; memcpy(bad, sae_peer_commit, sizeof bad); bad[40] ^= 1;   /* element off the curve */
    UASSERT(sae_process_commit(&s, bad, sizeof bad) != 0);
    UASSERT(sae_process_commit(&s, sae_local_commit, sizeof sae_local_commit) != 0);              /* our own commit reflected */
}
TEST(sae_two_sides)                                     /* a full exchange between two of our own instances, random rand / mask */
{
    static sae_t a, b; uint8_t ca[128], cb[128], fa[40], fb[40];
    UASSERT(sae_prepare(&a, sae_addr1, sae_addr2, "pass word", 9) == 0 && sae_prepare(&b, sae_addr2, sae_addr1, "pass word", 9) == 0);
    unsigned na = sae_write_commit(&a, ca, 0, 0), nb = sae_write_commit(&b, cb, 0, 0);
    UASSERT(sae_process_commit(&a, cb, nb) == 0 && sae_process_commit(&b, ca, na) == 0);
    UASSERT(!memcmp(a.pmk, b.pmk, 32) && !memcmp(a.pmkid, b.pmkid, 16));
    sae_write_confirm(&a, fa); sae_write_confirm(&b, fb);
    UASSERT(sae_check_confirm(&a, fb, 34) == 0 && sae_check_confirm(&b, fa, 34) == 0);
    static sae_t c; uint8_t cc[128];                    /* the wrong password: commits are accepted, the confirm is not */
    UASSERT(sae_prepare(&c, sae_addr2, sae_addr1, "pass worD", 9) == 0 && sae_prepare(&a, sae_addr1, sae_addr2, "pass word", 9) == 0);
    unsigned nc = sae_write_commit(&c, cc, 0, 0); na = sae_write_commit(&a, ca, 0, 0);
    UASSERT(sae_process_commit(&a, cc, nc) == 0 && sae_process_commit(&c, ca, na) == 0);
    sae_write_confirm(&c, fb); UASSERT(sae_check_confirm(&a, fb, 34) != 0);
}
