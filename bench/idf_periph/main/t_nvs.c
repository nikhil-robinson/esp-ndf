#include "t.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_rom_crc.h"
static uint8_t big[10000], back[10000];
static void pattern(uint8_t *b, int n, int seed) { for (int i = 0; i < n; i++) b[i] = (i * 7 + seed) ^ (i >> 5); }
void t_nvs(void)
{
    printf("    crc32_le(0xffffffff, \"123456789\") = %08lx\n", esp_rom_crc32_le(0xFFFFFFFF, (const uint8_t *)"123456789", 9));
    esp_err_t e = nvs_flash_init_partition("nvs");
    printf("    init: %s\n", esp_err_to_name(e));
    nvs_handle_t h; nvs_open_from_partition("nvs", "robot", NVS_READWRITE, &h);
    uint8_t a = 0; int32_t b = 0; uint64_t c = 0; uint32_t ctr = 0; char s[32] = "?"; size_t sl = sizeof s, bl;
    int ok = 0;
    ok += nvs_get_u8(h, "u8", &a) == ESP_OK && a == 0xAB;
    ok += nvs_get_i32(h, "i32", &b) == ESP_OK && b == -123456;
    ok += nvs_get_u64(h, "u64", &c) == ESP_OK && c == 0x1122334455667788ull;
    ok += nvs_get_str(h, "name", s, &sl) == ESP_OK && !strcmp(s, "esp-ndf robot v1");
    pattern(big, 10000, 1);
    bl = sizeof back; ok += nvs_get_blob(h, "cal3k", back, &bl) == ESP_OK && bl == 3000 && !memcmp(back, big, 3000);
    bl = sizeof back; ok += nvs_get_blob(h, "big10k", back, &bl) == ESP_OK && bl == 10000 && !memcmp(back, big, 10000);
    ok += nvs_get_u32(h, "counter", &ctr) == ESP_OK && ctr == 599;
    printf("    read esp-ndf data: %d/7 keys correct (name=\"%s\", counter=%lu)\n", ok, s, ctr);
    METRIC("nvs_get_u32()", "%lu cycles", CYCLES(nvs_get_u32(h, "counter", &ctr)));
    uint32_t t = ccount();
    for (uint32_t i = 0; i < 100; i++) nvs_set_u32(h, "counter2", i);
    printf("    100 updates: %lu us each\n", (ccount() - t) / 240 / 100);
    pattern(big, 5000, 3);
    nvs_set_u32(h, "from_idf", 0xC0FFEE);
    nvs_set_str(h, "idf_str", "hello from idf");
    nvs_set_blob(h, "idf_blob", big, 5000);
    nvs_commit(h); nvs_close(h);
    printf("    wrote from_idf / idf_str / idf_blob for esp-ndf\n");
}
