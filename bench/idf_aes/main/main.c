/* IDF sibling of components/drivers/crypto/test_apps/aes_test: mbedtls AES-128 CBC / CTR (hardware AES, IDF's DMA port), best of 4. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"
#include "esp_cpu.h"
static uint8_t A[65536], B[65536];
void app_main(void)
{
    vTaskDelay(300);
    mbedtls_aes_context c; mbedtls_aes_init(&c); uint8_t k[16] = { 1 }; mbedtls_aes_setkey_enc(&c, k, 128);
    static const size_t sz[] = { 16, 256, 4096, 65536 };
    for (int m = 0; m < 2; m++) for (int i = 0; i < 4; i++) {
        uint32_t best = ~0u;
        for (int r = 0; r < 4; r++) { uint8_t iv[16] = { 0 }, st[16]; size_t off = 0; uint32_t t = esp_cpu_get_cycle_count();
            if (m == 0) mbedtls_aes_crypt_cbc(&c, MBEDTLS_AES_ENCRYPT, sz[i], iv, A, B);
            else mbedtls_aes_crypt_ctr(&c, sz[i], &off, iv, st, A, B);
            t = esp_cpu_get_cycle_count() - t; if (t < best) best = t; }
        printf("idf speed AES-128-%s %5u B: %7lu cycles (%lu MB/s)\n", m ? "CTR" : "CBC", (unsigned)sz[i], (unsigned long)best, (unsigned long)((uint64_t)sz[i] * 240 / best));
    }
    printf("idf_aes done\n");
    for (;;) vTaskDelay(1000);
}
