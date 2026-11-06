/* IDF sibling of components/lib/test_apps/lib_p4: crc32 (ROM table) / md5 (PSA) / newlib printf, strtod, qsort in cycles. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_crc.h"
#include "psa/crypto.h"
#define NOW() esp_cpu_get_cycle_count()
#define CRC32(b, n) esp_crc32_le(0, b, n)
#define MD5(b, n) do { uint8_t d_[16]; size_t l_; psa_hash_compute(PSA_ALG_MD5, b, n, d_, sizeof d_, &l_); } while (0)
#define PRINT(name, v) printf("idf  %-22s %6lu cycles\n", name, (unsigned long)(v))
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
void app_main(void)
{
    vTaskDelay(300);
    psa_crypto_init();
    static uint8_t buf[4096]; for (int i = 0; i < 4096; i++) buf[i] = i * 7 + (i >> 8);
    static int arr[1000], arr0[1000]; unsigned s = 12345; for (int i = 0; i < 1000; i++) { s = s * 1664525u + 1013904223u; arr0[i] = (int)(s >> 8); }
    static char out[64]; volatile uint32_t sink = 0; uint32_t t, best;
#define BEST(name, stmt) do { best = ~0u; for (int r_ = 0; r_ < 8; r_++) { t = NOW(); stmt; t = NOW() - t; if (t < best) best = t; } PRINT(name, best); } while (0)
    BEST("crc32 4 KB", sink += CRC32(buf, 4096));
    BEST("md5 4 KB", MD5(buf, 4096));
    BEST("snprintf %d", snprintf(out, sizeof out, "%d", 123456));
    BEST("snprintf %.3f", snprintf(out, sizeof out, "%.3f", 3.14159265));
    BEST("snprintf %s %5d %x", snprintf(out, sizeof out, "%s %5d %x", "abc", 42, 255));
    BEST("strtod 3.14159265", sink += (uint32_t)strtod("3.14159265", 0));
    BEST("strtol 123456", sink += strtol("123456", 0, 10));
    BEST("qsort 1000 ints", (memcpy(arr, arr0, sizeof arr), qsort(arr, 1000, sizeof arr[0], cmp)));
    { volatile double da = 1.2345678901, db = 9.8765432109; volatile float fa = 1.2345f, fb = 9.8765f; volatile int64_t la = 1234567890123456789ll, lb = 987654321ll; volatile uint32_t ua = 123456789u, ub = 12345u; volatile double dr;
    BEST("double add x100", for (int i = 0; i < 100; i++) dr = da + db);
    BEST("double mul x100", for (int i = 0; i < 100; i++) dr = da * db);
    BEST("double div x100", for (int i = 0; i < 100; i++) dr = da / db);
    BEST("double sqrt x100", for (int i = 0; i < 100; i++) dr = sqrt(da));
    BEST("double sin x100", for (int i = 0; i < 100; i++) dr = sin(da + i));
    BEST("double exp x100", for (int i = 0; i < 100; i++) dr = exp(da + i * 0.01));
    BEST("double->int x100", for (int i = 0; i < 100; i++) sink += (int)(da * i));
    BEST("float div x100", for (int i = 0; i < 100; i++) sink += (int)(fa / fb));
    BEST("int64 div x100", for (int i = 0; i < 100; i++) sink += (uint32_t)(la / (lb + i)));
    BEST("uint32 div x100", for (int i = 0; i < 100; i++) sink += ua / (ub + i)); }
    { volatile double da = 1.2345678901, db = 9.8765432109; volatile double dr;
    BEST("dbl sin small x100", for (int i = 0; i < 100; i++) dr = sin(0.5 + i * 0.001));
    BEST("dbl cos x100", for (int i = 0; i < 100; i++) dr = cos(da + i));
    BEST("dbl tan x100", for (int i = 0; i < 100; i++) dr = tan(0.5 + i * 0.01));
    BEST("dbl atan x100", for (int i = 0; i < 100; i++) dr = atan(da + i));
    BEST("dbl log x100", for (int i = 0; i < 100; i++) dr = log(da + i));
    BEST("dbl pow x100", for (int i = 0; i < 100; i++) dr = pow(da + i, 0.37));
    BEST("dbl fmod x100", for (int i = 0; i < 100; i++) dr = fmod(db * (i + 1), da));
    BEST("dbl floor x100", for (int i = 0; i < 100; i++) dr = floor(db * i));
    BEST("dbl sqrt x100 (again)", for (int i = 0; i < 100; i++) dr = sqrt(db + i)); (void)dr; }
    printf("idf_p4_lib done\n");
    for (;;) vTaskDelay(1000);
}
