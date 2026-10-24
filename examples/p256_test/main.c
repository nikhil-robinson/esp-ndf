/* P-256: public key, ECDH, ECDSA sign (RFC 6979 k) and verify against bench/p256/vectors.h (python cryptography);
 * random keygen/sign/verify round trip, tampered signature and off-curve key rejected. Sibling: bench/idf_p256. */
#include "console.h"
#include "p256.h"
#include "sys.h"
#include "xtensa.h"
#include <string.h>
#include "../../bench/p256/vectors.h"

static uint32_t t0;
static void tic(void) { t0 = ccount(); }
static unsigned long toc(void) { return (ccount() - t0) / (sys_cpu_hz() / 1000000); }

int main(void)
{
    delay_ms(200);
    uint8_t pub[64], sh[32], sig[64], pr[32], pu[64];
    tic(); int ok1 = !p256_pubkey(pub, v_priv) && !memcmp(pub, v_pub, 64); unsigned long t1 = toc();
    tic(); int ok2 = !p256_ecdh(sh, v_priv, v_pub2) && !memcmp(sh, v_shared, 32); unsigned long t2 = toc();
    tic(); int ok3 = !p256_sign_k(sig, v_priv, v_hash, v_k) && !memcmp(sig, v_sig_k, 64); unsigned long t3 = toc();
    tic(); int ok4 = !p256_verify(v_pub, v_hash, v_sig_py); unsigned long t4 = toc();
    p256_keygen(pr, pu); p256_sign(sig, pr, v_hash);
    int ok5 = !p256_verify(pu, v_hash, sig);
    sig[5] ^= 1; int ok6 = p256_verify(pu, v_hash, sig) != 0;
    pu[63] ^= 1; int ok7 = p256_check_pub(pu) != 0;
    int pass = ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7;
    printf("%s P-256: pubkey %d, ecdh %d, sign %d, verify %d, roundtrip %d, tamper %d, off-curve %d | "
           "pubkey %lu us, ecdh %lu us, sign %lu us, verify %lu us\n", pass ? "PASS" : "FAIL",
           ok1, ok2, ok3, ok4, ok5, ok6, ok7, t1, t2, t3, t4);
    printf("p256_test %s done\n", pass ? "PASS" : "FAIL");
    for (;;) {}
}
