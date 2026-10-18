#include "t.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_cpu.h"
#include <string.h>
static uint8_t buf[8000];
void t_nvs_enc(void)
{
    const esp_partition_t *kp = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, "nvs_keys");
    nvs_sec_cfg_t cfg;
    if (!kp || esp_partition_read(kp, 0, &cfg, sizeof cfg)) { printf("    no keys\n"); return; }
    esp_err_t e = nvs_flash_secure_init_partition("nvs_enc", &cfg);   /* keys passed directly: no eFuse/HMAC involved */
    printf("    secure init: %s\n", esp_err_to_name(e));
    if (e) return;
    nvs_handle_t h; uint32_t u = 0; char s[48]; size_t sl = sizeof s, bl = sizeof buf;
    if (nvs_open_from_partition("nvs_enc", "gen", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "u32", &u); nvs_get_str(h, "str", s, &sl); nvs_get_blob(h, "blob", buf, &bl);
        printf("    host image: u32 %lu, str \"%s\", blob %u B crc32 %08lx\n", u, s, bl, esp_rom_crc32_le(0, buf, bl));
        nvs_close(h);
    }
    if (nvs_open_from_partition("nvs_enc", "ndf", NVS_READONLY, &h) == ESP_OK) {
        sl = sizeof s; bl = sizeof buf;
        esp_err_t a = nvs_get_str(h, "secret", s, &sl), b = nvs_get_blob(h, "blob6k", buf, &bl), c = nvs_get_u32(h, "ctr", &u);
        bool ok = bl == 6000;
        for (int i = 0; i < 6000 && ok; i++) ok = buf[i] == (uint8_t)((i * 7 + 7) ^ (i >> 5));
        printf("    esp-ndf: secret \"%s\" (%s), blob6k %s, ctr %lu (%s)\n", s, esp_err_to_name(a), ok && !b ? "ok" : "BAD", u, esp_err_to_name(c));
        uint32_t t = esp_cpu_get_cycle_count(); nvs_get_u32(h, "ctr", &u);
        printf("    nvs_get_u32() encrypted          %lu cycles\n", esp_cpu_get_cycle_count() - t);
        nvs_close(h);
    }
    if (nvs_open_from_partition("nvs_enc", "idf", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "from_idf", "IDF wrote this, encrypted");
        uint32_t t = esp_cpu_get_cycle_count();
        for (int i = 0; i < 100; i++) nvs_set_u32(h, "ctr", 1000 + i);
        printf("    100 encrypted updates: %lu us each\n", (esp_cpu_get_cycle_count() - t) / 240 / 100);
        nvs_commit(h); nvs_close(h);
    }
    nvs_flash_deinit_partition("nvs_enc");
}
