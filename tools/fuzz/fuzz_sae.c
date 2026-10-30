/* SAE (components/wifi/sae.c) under ASan / UBSan: the 802.11-2020 Annex J.10 vector, then the AP side's commit and confirm as attacker
 * input: random bodies, a real commit with bytes flipped / cut, an element off the curve, a zero / order-sized scalar. Nothing may crash,
 * and a confirm built from a tampered commit must never be accepted. usage: fuzz_sae <rounds> <seed> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sae.h"
#include "../tls_shims.c"
#include <openssl/evp.h>
#include "../../components/wifi/test_apps/wpa_unit/sae_vectors.h"

void aes_block(const uint8_t *key, int kl, bool enc, const uint8_t in[16], uint8_t out[16])   /* OpenSSL AES-128 ECB (wpa_sha256.c's CMAC) */
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); int n;
    EVP_CipherInit_ex(c, kl == 32 ? EVP_aes_256_ecb() : EVP_aes_128_ecb(), 0, key, 0, enc); EVP_CIPHER_CTX_set_padding(c, 0);
    EVP_CipherUpdate(c, out, &n, in, 16); EVP_CIPHER_CTX_free(c);
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)atoi(argv[1]) : 200; srand(argc > 2 ? (unsigned)atoi(argv[2]) : 1);
    static sae_t a, b; uint8_t ca[128], cb[128], fa[40], fb[40], m[200];
    CHECK(sae_prepare_fixed(&a, sae_addr1, sae_addr2, "mekmitasdigoat", 14, sae_local_rand, sae_local_mask) == 0);
    CHECK(sae_write_commit(&a, ca, 0, 0) == sizeof sae_local_commit && !memcmp(ca, sae_local_commit, sizeof sae_local_commit));
    CHECK(sae_process_commit(&a, sae_peer_commit, sizeof sae_peer_commit) == 0);
    CHECK(!memcmp(a.kck, sae_kck, 32) && !memcmp(a.pmk, sae_pmk, 32) && !memcmp(a.pmkid, sae_pmkid, 16));
    static const uint8_t n_be[32] = { 0xff,0xff,0xff,0xff,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51 };
    memcpy(m, sae_peer_commit, sizeof sae_peer_commit); memset(m + 2, 0, 32); CHECK(sae_process_commit(&a, m, sizeof sae_peer_commit) != 0);      /* scalar 0 */
    memcpy(m + 2, n_be, 32); CHECK(sae_process_commit(&a, m, sizeof sae_peer_commit) != 0);                                                     /* scalar = r */
    memcpy(m, sae_peer_commit, sizeof sae_peer_commit); m[0] = 20; CHECK(sae_process_commit(&a, m, sizeof sae_peer_commit) != 0);              /* group 20 */
    for (unsigned c = 0; c < sizeof sae_peer_commit; c++) CHECK(sae_process_commit(&a, sae_peer_commit, c) != 0);                              /* every cut */
    unsigned accepted = 0;
    for (unsigned r = 0; r < rounds; r++) {
        CHECK(sae_prepare(&a, sae_addr1, sae_addr2, "pw", 2) == 0 && sae_prepare(&b, sae_addr2, sae_addr1, "pw", 2) == 0);
        unsigned na = sae_write_commit(&a, ca, 0, 0), nb = sae_write_commit(&b, cb, 0, 0);
        memcpy(m, cb, nb); unsigned nm = nb;
        switch (rand() % 4) {
        case 0: for (unsigned i = 0; i < nm; i++) m[i] = (uint8_t)rand(); m[0] = 19; m[1] = 0; break;   /* random scalar and element */
        case 1: m[2 + rand() % (nm - 2)] ^= (uint8_t)(1u << (rand() % 8)); break;                       /* one bit of a real commit */
        case 2: nm = (unsigned)(rand() % (int)nb); break;                                              /* cut short */
        default: nm = nb + (unsigned)(rand() % 50); for (unsigned i = nb; i < nm; i++) m[i] = (uint8_t)rand();   /* trailing bytes (an IE) */
        }
        int tampered = nm < nb || memcmp(m, cb, nb);
        if (sae_process_commit(&a, m, nm)) continue;
        sae_process_commit(&b, ca, na);
        sae_write_confirm(&b, fb); sae_write_confirm(&a, fa);
        for (unsigned i = 0; i < 34; i++) m[i] = (uint8_t)rand();
        CHECK(sae_check_confirm(&a, m, (unsigned)(rand() % 40)) != 0);                                 /* a random confirm */
        if (!sae_check_confirm(&a, fb, 34)) { accepted++; CHECK(!tampered); }                          /* only an untouched commit ends in an accepted confirm */
    }
    printf("fuzz_sae: %u rounds, %u honest exchanges accepted, %s\n", rounds, accepted, fails ? "FAILED" : "no memory error, no false accept");
    return fails ? 1 : 0;
}
