/* IDF sibling of examples/x25519_test: mbedtls Curve25519 (ecp_mul on the Montgomery curve) on the same vectors. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/ecp.h"
#include "esp_cpu.h"
#include "esp_random.h"
#include "vectors.h"
static int rnd(void *c, unsigned char *b, size_t n) { esp_fill_random(b, n); return 0; }
void app_main(void)
{
    vTaskDelay(300);
    mbedtls_ecp_group g; mbedtls_ecp_group_init(&g); mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_CURVE25519);
    mbedtls_mpi d; mbedtls_ecp_point P, R; mbedtls_mpi_init(&d); mbedtls_ecp_point_init(&P); mbedtls_ecp_point_init(&R);
    uint8_t o[32]; size_t ol;
    mbedtls_mpi_read_binary_le(&d, v_a, 32);                  /* clamp as RFC 7748 */
    mbedtls_mpi_set_bit(&d, 0, 0); mbedtls_mpi_set_bit(&d, 1, 0); mbedtls_mpi_set_bit(&d, 2, 0); mbedtls_mpi_set_bit(&d, 255, 0); mbedtls_mpi_set_bit(&d, 254, 1);
    uint32_t t = esp_cpu_get_cycle_count(); mbedtls_ecp_mul(&g, &R, &d, &g.G, rnd, NULL); uint32_t t1 = esp_cpu_get_cycle_count() - t;
    mbedtls_ecp_point_write_binary(&g, &R, MBEDTLS_ECP_PF_COMPRESSED, &ol, o, 32); int ok1 = !memcmp(o, v_apub, 32);
    mbedtls_ecp_point_read_binary(&g, &P, v_bpub, 32);
    t = esp_cpu_get_cycle_count(); mbedtls_ecp_mul(&g, &R, &d, &P, rnd, NULL); uint32_t t2 = esp_cpu_get_cycle_count() - t;
    mbedtls_ecp_point_write_binary(&g, &R, MBEDTLS_ECP_PF_COMPRESSED, &ol, o, 32); int ok2 = !memcmp(o, v_shared, 32);
    printf("idf X25519: pubkey %d, shared %d | pubkey %lu us, shared %lu us\n", ok1, ok2, (unsigned long)(t1 / 240), (unsigned long)(t2 / 240));
    printf("idf_x25519 done\n");
    for (;;) vTaskDelay(1000);
}
