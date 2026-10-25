/* IDF sibling of components/drivers/crypto/test_apps/gcm_test: mbedtls_gcm (hardware AES port) on the same vectors. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/gcm.h"
#include "esp_cpu.h"
#include "vectors.h"
static uint8_t buf[16384], pt[16384];
void app_main(void)
{
    vTaskDelay(300);
    for (unsigned v = 0; v < sizeof gcm_vec / sizeof gcm_vec[0]; v++) {
        const gcm_vec_t *c = &gcm_vec[v];
        uint8_t key[32], iv[16], aad[20], tag[16];
        for (int i = 0; i < c->kl; i++) key[i] = i + 1;
        for (int i = 0; i < c->il; i++) iv[i] = 0xa0 + i;
        for (int i = 0; i < c->al; i++) aad[i] = i ^ 0x5a;
        for (int i = 0; i < c->len; i++) pt[i] = (i * 7 + 3) & 255;
        mbedtls_gcm_context g; mbedtls_gcm_init(&g);
        uint32_t t = esp_cpu_get_cycle_count();
        mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, c->kl * 8);
        mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, c->len, iv, c->il, aad, c->al, pt, buf, 16, tag);
        uint32_t te = esp_cpu_get_cycle_count() - t;
        int ok = !memcmp(tag, c->tag, 16);
        t = esp_cpu_get_cycle_count();
        mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, c->kl * 8);
        int rd = mbedtls_gcm_auth_decrypt(&g, c->len, iv, c->il, aad, c->al, tag, 16, buf, buf);
        uint32_t td = esp_cpu_get_cycle_count() - t;
        printf("idf GCM-%d iv %d aad %d len %5d: tag %d, decrypt %d | encrypt %lu us, decrypt %lu us\n", c->kl * 8, c->il, c->al,
               c->len, ok, !rd && !memcmp(buf, pt, c->len), (unsigned long)(te / 240), (unsigned long)(td / 240));
        mbedtls_gcm_free(&g);
    }
    printf("idf_gcm done\n");
    for (;;) vTaskDelay(1000);
}
