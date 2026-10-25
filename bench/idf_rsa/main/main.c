/* IDF sibling of components/drivers/crypto/test_apps/rsa_test: mbedtls_mpi_exp_mod (hardware MPI) on the same vectors, with RR cached (as an RSA
 * key keeps it) and without, plus the RR preparation. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/bignum.h"
#include "esp_cpu.h"
#include "vectors.h"
static void set(mbedtls_mpi *v, const uint32_t *w, int n) { mbedtls_mpi_grow(v, n); for (int i = 0; i < n; i++) v->MBEDTLS_PRIVATE(p)[i] = w[i]; v->MBEDTLS_PRIVATE(s) = 1; }
static void one(int bits, const uint32_t *m, const uint32_t *x, const uint32_t *d, const uint32_t *pub, const uint32_t *priv)
{
    int n = bits / 32; mbedtls_mpi M, X, D, E, Z, RR, W; mbedtls_mpi_init(&M); mbedtls_mpi_init(&X); mbedtls_mpi_init(&D); mbedtls_mpi_init(&E); mbedtls_mpi_init(&Z); mbedtls_mpi_init(&RR); mbedtls_mpi_init(&W);
    set(&M, m, n); set(&X, x, n); set(&D, d, n); mbedtls_mpi_lset(&E, 65537);
    uint32_t t = esp_cpu_get_cycle_count(); mbedtls_mpi_exp_mod(&Z, &X, &E, &M, &RR); uint32_t tfirst = esp_cpu_get_cycle_count() - t;
    set(&W, pub, n); int ok1 = !mbedtls_mpi_cmp_mpi(&Z, &W);
    t = esp_cpu_get_cycle_count(); mbedtls_mpi_exp_mod(&Z, &X, &E, &M, &RR); uint32_t tpub = esp_cpu_get_cycle_count() - t;
    t = esp_cpu_get_cycle_count(); mbedtls_mpi_exp_mod(&Z, &X, &D, &M, &RR); uint32_t tpriv = esp_cpu_get_cycle_count() - t;
    set(&W, priv, n); int ok2 = !mbedtls_mpi_cmp_mpi(&Z, &W);
    printf("idf RSA-%d: ok %d %d | first x^65537 (computes RR) %lu us, x^65537 %lu us, x^d %lu us\n", bits, ok1, ok2,
           (unsigned long)(tfirst / 240), (unsigned long)(tpub / 240), (unsigned long)(tpriv / 240));
}
void app_main(void)
{
    vTaskDelay(300);
    one(1024, v1024_m, v1024_x, v1024_d, v1024_pub, v1024_priv);
    one(2048, v2048_m, v2048_x, v2048_d, v2048_pub, v2048_priv);
    one(4096, v4096_m, v4096_x, v4096_d, v4096_pub, v4096_priv);
    printf("idf_rsa done\n");
    for (;;) vTaskDelay(1000);
}
