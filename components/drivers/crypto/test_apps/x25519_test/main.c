/* X25519: public key, shared secret (python cryptography) and RFC 7748 5.2 vector 1; timing (sibling: bench/idf_x25519). */
#include "console.h"
#include "x25519.h"
#include "sys.h"
#include "cpu.h"
#include <string.h>
#include "x25519/vectors.h"

int main(void)
{
    delay_ms(200);
    uint8_t o[32];
    uint32_t t = ccount(); x25519_base(o, v_a); uint32_t t1 = ccount() - t; int ok1 = !memcmp(o, v_apub, 32);
    t = ccount(); x25519(o, v_a, v_bpub); uint32_t t2 = ccount() - t; int ok2 = !memcmp(o, v_shared, 32);
    x25519(o, v_rk, v_ru); int ok3 = !memcmp(o, v_ro, 32);
    int pass = ok1 && ok2 && ok3;
    printf("%s X25519: pubkey %d, shared %d, rfc7748 %d | pubkey %lu us, shared %lu us\n", pass ? "PASS" : "FAIL", ok1, ok2, ok3,
           (unsigned long)(t1 / 240), (unsigned long)(t2 / 240));
    printf("x25519_test %s done\n", pass ? "PASS" : "FAIL");
    for (;;) {}
}
