#include "console.h"
#include "ecc_hw.h"
#include "ecdsa_hw.h"
#include "p256.h"
#include "sys.h"
#include "cpu.h"
#include <string.h>
#include "p256/vectors.h"

static void rev(uint8_t *o, const uint8_t *i, int n) { for (int k = 0; k < n; k++) o[k] = i[n - 1 - k]; }

int main(void)
{
    delay_ms(100);
    int ok[8] = { 0 };
    uint8_t kl[32], pxl[32], pyl[32], rx[32], ry[32], sw[64], pt[64];
    rev(kl, v_priv, 32); rev(pxl, v_pub2, 32); rev(pyl, v_pub2 + 32, 32);
    uint32_t t = ccount();
    int r = ecc_hw_mul(ECC_P256, kl, pxl, pyl, rx, ry);
    uint32_t us = (ccount() - t) / (sys_cpu_hz() / 1000000);
    p256_point_mul(sw, v_priv, v_pub2);
    rev(pt, rx, 32); rev(pt + 32, ry, 32);
    ok[0] = r == 0 && !memcmp(pt, sw, 64);
    rev(pxl, v_pub, 32); rev(pyl, v_pub + 32, 32);
    ok[1] = ecc_hw_verify(ECC_P256, pxl, pyl) == 1;
    pyl[0] ^= 1; ok[2] = ecc_hw_verify(ECC_P256, pxl, pyl) == 0; pyl[0] ^= 1;
    uint8_t qxl[32], qyl[32], sum[64], hs[64];
    rev(qxl, v_pub2, 32); rev(qyl, v_pub2 + 32, 32);
    r = ecc_hw_add(ECC_P256, pxl, pyl, qxl, qyl, rx, ry);
    p256_point_add(sum, v_pub, v_pub2);
    rev(hs, rx, 32); rev(hs + 32, ry, 32);
    ok[3] = r == 0 && !memcmp(hs, sum, 64);
    uint8_t a[32] = { 5 }, b[32] = { 7 }, m[32];
    ecc_hw_mod(ECC_P256, ECC_MOD_MUL, false, a, b, m); ok[4] = m[0] == 35 && !m[1];
    ecc_hw_mod(ECC_P256, ECC_MOD_INV, false, a, 0, m);
    uint8_t chk[32]; ecc_hw_mod(ECC_P256, ECC_MOD_MUL, false, a, m, chk); ok[5] = chk[0] == 1 && !chk[1] && !chk[31];
    uint8_t hl[32], rl[32], sl[32];
    rev(hl, v_hash, 32); rev(rl, v_sig_py, 32); rev(sl, v_sig_py + 32, 32);
    t = ccount();
    ok[6] = ecdsa_hw_verify(ECC_P256, hl, rl, sl, pxl, pyl) == 0;
    uint32_t us2 = (ccount() - t) / (sys_cpu_hz() / 1000000);
    sl[3] ^= 1; ok[7] = ecdsa_hw_verify(ECC_P256, hl, rl, sl, pxl, pyl) != 0;
    int pass = 1; for (int i = 0; i < 8; i++) pass &= ok[i];
    printf("%s ECC: mul %d verify %d off-curve %d add %d modmul %d modinv %d | ECDSA verify %d tampered-rejected %d | mul %lu us, verify %lu us\n",
           pass ? "PASS" : "FAIL", ok[0], ok[1], ok[2], ok[3], ok[4], ok[5], ok[6], ok[7], (unsigned long)us, (unsigned long)us2);
    printf("ecc_p4 %s done\n", pass ? "PASS" : "FAIL");
    for (;;) {}
}
