/* IDF sibling of components/drivers/crypto/test_apps/sha_test: mbedtls SHA-256 / SHA-512 with the hardware accelerator (IDF default port; on the
 * S3 the DMA port), best of 4, same sizes. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "esp_cpu.h"
static uint8_t big[65536];
void app_main(void)
{
    vTaskDelay(300);
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7 + (i >> 9));
    static const size_t sz[] = { 64, 1024, 4096, 65536 };
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 4; i++) {
            uint8_t d[64]; uint32_t best = ~0u;
            for (int r = 0; r < 4; r++) { uint32_t c = esp_cpu_get_cycle_count();
                if (t) mbedtls_sha512(big, sz[i], d, 0); else mbedtls_sha256(big, sz[i], d, 0);
                c = esp_cpu_get_cycle_count() - c; if (c < best) best = c; }
            printf("idf speed %s %6u B: %7lu cycles (%lu us, %lu MB/s)\n", t ? "SHA-512" : "SHA-256", (unsigned)sz[i],
                   (unsigned long)best, (unsigned long)(best / 240), (unsigned long)((uint64_t)sz[i] * 240 / best));
        }
    printf("idf_sha done\n");
    for (;;) vTaskDelay(1000);
}
