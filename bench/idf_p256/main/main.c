/* IDF sibling of components/drivers/crypto/test_apps/p256_test: mbedtls ECP (default config) on the same vectors. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecdh.h"
#include "esp_cpu.h"
#include "esp_random.h"
#include "vectors.h"
static int rnd(void *c, unsigned char *b, size_t n) { esp_fill_random(b, n); return 0; }
static uint32_t t0; static void tic(void) { t0 = esp_cpu_get_cycle_count(); } static unsigned long toc(void) { return (esp_cpu_get_cycle_count() - t0) / 240; }
void app_main(void)
{
    vTaskDelay(300);
    mbedtls_ecp_group g; mbedtls_ecp_group_init(&g); mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1);
    mbedtls_mpi d, d2, z, r, s; mbedtls_ecp_point Q, Q2, R;
    mbedtls_mpi_init(&d); mbedtls_mpi_init(&d2); mbedtls_mpi_init(&z); mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
    mbedtls_ecp_point_init(&Q); mbedtls_ecp_point_init(&Q2); mbedtls_ecp_point_init(&R);
    mbedtls_mpi_read_binary(&d, v_priv, 32); mbedtls_mpi_read_binary(&d2, v_priv2, 32);
    mbedtls_ecp_mul(&g, &Q2, &d2, &g.G, rnd, NULL);               /* warm up (fixed-point table for G) */
    tic(); mbedtls_ecp_mul(&g, &Q, &d, &g.G, rnd, NULL); unsigned long t1 = toc();
    uint8_t buf[65]; size_t ol; mbedtls_ecp_point_write_binary(&g, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &ol, buf, 65);
    int ok1 = !memcmp(buf + 1, v_pub, 64);
    tic(); mbedtls_ecdh_compute_shared(&g, &z, &Q2, &d, rnd, NULL); unsigned long t2 = toc();
    uint8_t sh[32]; mbedtls_mpi_write_binary(&z, sh, 32); int ok2 = !memcmp(sh, v_shared, 32);
    tic(); mbedtls_ecdsa_sign(&g, &r, &s, &d, v_hash, 32, rnd, NULL); unsigned long t3 = toc();
    mbedtls_mpi_read_binary(&r, v_sig_py, 32); mbedtls_mpi_read_binary(&s, v_sig_py + 32, 32);
    tic(); int ok4 = !mbedtls_ecdsa_verify(&g, v_hash, 32, &Q, &r, &s); unsigned long t4 = toc();
    printf("idf P-256: pubkey %d, ecdh %d, verify %d | pubkey %lu us, ecdh %lu us, sign %lu us, verify %lu us\n", ok1, ok2, ok4, t1, t2, t3, t4);
    printf("idf_p256 done\n");
    for (;;) vTaskDelay(1000);
}
