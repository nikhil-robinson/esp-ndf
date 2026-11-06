/* IDF sibling of the crypto tests on the P4 (ESP-IDF v6.1: mbedtls 4 / PSA; AES and GCM through the port's own esp_aes API, hardware AES and SHA with
 * their GDMA path): AES-128 CBC / CTR, SHA-256 / SHA-512, AES-GCM and P-256 on the same vectors as components/drivers/crypto/test_apps. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "aes/esp_aes.h"
#include "aes/esp_aes_gcm.h"
#include "psa/crypto.h"
#include "esp_cpu.h"
#include "gcm/vectors.h"
#include "p256/vectors.h"
static uint8_t A[65536], B[65536];
static void aes_bench(void)
{
    esp_aes_context c; esp_aes_init(&c); uint8_t k[16] = { 1 }; esp_aes_setkey(&c, k, 128);
    static const size_t sz[] = { 16, 256, 4096, 65536 };
    for (int m = 0; m < 2; m++) for (int i = 0; i < 4; i++) {
        uint32_t best = ~0u;
        for (int r = 0; r < 4; r++) { uint8_t iv[16] = { 0 }, st[16]; size_t off = 0; uint32_t t = esp_cpu_get_cycle_count();
            if (m == 0) esp_aes_crypt_cbc(&c, ESP_AES_ENCRYPT, sz[i], iv, A, B);
            else esp_aes_crypt_ctr(&c, sz[i], &off, iv, st, A, B);
            t = esp_cpu_get_cycle_count() - t; if (t < best) best = t; }
        printf("idf speed AES-128-%s %5u B: %7lu cycles\n", m ? "CTR" : "CBC", (unsigned)sz[i], (unsigned long)best);
    }
}
static void sha_bench(void)
{
    static const size_t sz[] = { 64, 1024, 4096, 65536 };
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 4; i++) {
            uint8_t d[64]; uint32_t best = ~0u;
            for (int r = 0; r < 4; r++) { uint32_t c = esp_cpu_get_cycle_count(); size_t dl;
                psa_hash_compute(t ? PSA_ALG_SHA_512 : PSA_ALG_SHA_256, A, sz[i], d, sizeof d, &dl);
                c = esp_cpu_get_cycle_count() - c; if (c < best) best = c; }
            printf("idf speed %s %6u B: %7lu cycles\n", t ? "SHA-512" : "SHA-256", (unsigned)sz[i], (unsigned long)best);
        }
}
static uint8_t gbuf[16384], gpt[16384];
static void gcm_bench(void)
{
    for (unsigned v = 0; v < sizeof gcm_vec / sizeof gcm_vec[0]; v++) {
        const gcm_vec_t *c = &gcm_vec[v];
        uint8_t key[32], iv[16], aad[20], tag[16];
        for (int i = 0; i < c->kl; i++) key[i] = i + 1;
        for (int i = 0; i < c->il; i++) iv[i] = 0xa0 + i;
        for (int i = 0; i < c->al; i++) aad[i] = i ^ 0x5a;
        for (int i = 0; i < c->len; i++) gpt[i] = (i * 7 + 3) & 255;
        esp_gcm_context g; esp_aes_gcm_init(&g);
        uint32_t best = ~0u, ok = 0;
        for (int r = 0; r < 3; r++) {
            uint32_t t = esp_cpu_get_cycle_count();
            esp_aes_gcm_setkey(&g, 2, key, c->kl * 8);
            esp_aes_gcm_crypt_and_tag(&g, 1, c->len, iv, c->il, aad, c->al, gpt, gbuf, 16, tag);
            t = esp_cpu_get_cycle_count() - t; if (t < best) best = t; ok = !memcmp(tag, c->tag, 16);
        }
        printf("idf GCM-%d iv %d aad %d len %5d: tag %lu | encrypt %lu cycles\n", c->kl * 8, c->il, c->al, c->len, (unsigned long)ok, (unsigned long)best);
        esp_aes_gcm_free(&g);
    }
}
static uint32_t t0; static void tic(void) { t0 = esp_cpu_get_cycle_count(); } static unsigned long toc(void) { return (esp_cpu_get_cycle_count() - t0) / 360; }
static psa_key_id_t imp(const uint8_t *k, size_t n, psa_key_type_t type, psa_key_usage_t use, psa_algorithm_t alg)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, type); psa_set_key_bits(&a, 256); psa_set_key_usage_flags(&a, use); psa_set_key_algorithm(&a, alg);
    psa_key_id_t id = 0; psa_import_key(&a, k, n, &id); return id;
}
static void p256_bench(void)
{

        psa_key_type_t pair = PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1), pub = PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1);
    psa_algorithm_t ecdsa = PSA_ALG_ECDSA(PSA_ALG_SHA_256);
    psa_key_id_t kd = imp(v_priv, 32, pair, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT, ecdsa);
    psa_key_id_t kd2 = imp(v_priv2, 32, pair, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_DERIVE, PSA_ALG_ECDH);
    uint8_t p2[65], buf[65], sh[32], sig[64]; size_t ol;
    psa_export_public_key(kd2, p2, sizeof p2, &ol);                /* warm up */
    tic(); psa_export_public_key(kd, buf, sizeof buf, &ol); unsigned long t1 = toc();
    int ok1 = !memcmp(buf + 1, v_pub, 64);
    psa_key_id_t kdh = imp(v_priv, 32, pair, PSA_KEY_USAGE_DERIVE, PSA_ALG_ECDH);
    tic(); psa_status_t se = psa_raw_key_agreement(PSA_ALG_ECDH, kdh, p2, 65, sh, sizeof sh, &ol); unsigned long t2 = toc();
    int ok2 = se == PSA_SUCCESS && !memcmp(sh, v_shared, 32);
    tic(); psa_sign_hash(kd, ecdsa, v_hash, 32, sig, sizeof sig, &ol); unsigned long t3 = toc();
    psa_key_id_t kp = imp(buf, 65, pub, PSA_KEY_USAGE_VERIFY_HASH, ecdsa);
    tic(); psa_status_t sv = psa_verify_hash(kp, ecdsa, v_hash, 32, v_sig_py, 64); unsigned long t4 = toc();
    printf("idf P-256: pubkey %d, ecdh %d, verify %d | pubkey %lu us, ecdh %lu us, sign %lu us, verify %lu us\n", ok1, ok2, sv == PSA_SUCCESS, t1, t2, t3, t4);
}
void app_main(void)
{
    vTaskDelay(300);
    psa_crypto_init();
    for (size_t i = 0; i < sizeof A; i++) A[i] = (uint8_t)(i * 7 + (i >> 9));
    aes_bench(); sha_bench(); gcm_bench(); p256_bench();
    printf("idf_p4_crypto done\n");
    for (;;) vTaskDelay(1000);
}
