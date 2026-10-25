/* The P-256 fault guard: a bit flipped in k G or in s must make p256_sign refuse (no signature leaves), and an
 * unfaulted run must still sign and verify. */
#include "unit.h"
#include "p256.h"
#include <string.h>

extern int p256_test_fault;

static const uint8_t priv[32] = { 0xC9,0xAF,0xA9,0xD8,0x45,0xBA,0x75,0x16,0x6B,0x5C,0x21,0x57,0x67,0xB1,0xD6,0x93,
                                  0x4E,0x50,0xC3,0xDB,0x36,0xE8,0x9B,0x12,0x7B,0x8A,0x62,0x2B,0x12,0x0F,0x67,0x21 };
static const uint8_t hash[32] = { 0xAF,0x2B,0xDB,0xE1,0xAA,0x9B,0x6E,0xC1,0xE2,0xAD,0xE1,0xD6,0x94,0xF4,0x1F,0xC7,
                                  0x1A,0x83,0x1D,0x02,0x68,0xE9,0x89,0x15,0x62,0x11,0x3D,0x8A,0x62,0xAD,0xD1,0xBF };

TEST(sign_ok_without_fault)
{
    uint8_t sig[64], pub[64];
    p256_test_fault = 0;
    UASSERT_EQ(p256_pubkey(pub, priv), 0);
    UASSERT_EQ(p256_sign(sig, priv, hash), 0);
    UASSERT_EQ(p256_verify(pub, hash, sig), 0);
}

TEST(fault_in_kG_refused)
{
    uint8_t sig[64]; memset(sig, 0x5A, sizeof sig);
    p256_test_fault = 1;
    int rc = p256_sign(sig, priv, hash);
    p256_test_fault = 0;
    UASSERT(rc != 0);
    for (int i = 0; i < 64; i++) UASSERT(sig[i] == 0 || sig[i] == 0x5A);     /* never a half-made signature with live data */
    UASSERT_EQ(rc, -1);
}

TEST(fault_in_s_refused)
{
    uint8_t sig[64];
    p256_test_fault = 2;
    int rc = p256_sign(sig, priv, hash);
    p256_test_fault = 0;
    UASSERT_EQ(rc, -1);
}
