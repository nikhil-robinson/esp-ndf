#include "t.h"
#include "esp_spiffs.h"
#include "esp_cpu.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#define BIG 30000
static uint8_t b[BIG + 16];
static uint8_t pat(uint32_t i) { return (uint8_t)(i * 7 + (i >> 9)); }
void t_spiffs(void)
{
    esp_vfs_spiffs_conf_t c = { .base_path = "/spiffs", .partition_label = "spiffs", .max_files = 4 };
    esp_err_t e = esp_vfs_spiffs_register(&c);
    printf("    mount: %s\n", esp_err_to_name(e));
    if (e) return;
    DIR *d = opendir("/spiffs"); struct dirent *de;
    while (d && (de = readdir(d))) {
        char p[300]; struct stat st; snprintf(p, sizeof p, "/spiffs/%s", de->d_name); stat(p, &st);
        printf("      %s  %ld\n", de->d_name, (long)st.st_size);
    }
    if (d) closedir(d);
    FILE *f = fopen("/spiffs/data/big.bin", "rb");
    uint32_t t = esp_cpu_get_cycle_count();
    size_t n = f ? fread(b, 1, sizeof b, f) : 0;
    uint32_t r = esp_cpu_get_cycle_count() - t;
    if (f) fclose(f);
    bool ok = n == BIG + 3 && !memcmp(b + BIG, "END", 3) && b[15060] == 'Y';
    for (uint32_t i = 0; i < 15059 && ok; i++) ok = b[i] == pat(i);
    printf("    esp-ndf big.bin: %u B, content %s, read %lu ms\n", n, ok ? "ok" : "BAD", r / 240000);
    char s[40] = { 0 }; f = fopen("/spiffs/hello.txt", "r"); if (f) { fread(s, 1, 39, f); fclose(f); }
    printf("    hello.txt: \"%s\"\n", s);

    for (uint32_t i = 0; i < BIG; i++) b[i] = pat(i);
    t = esp_cpu_get_cycle_count();
    f = fopen("/spiffs/idf_big.bin", "wb"); fwrite(b, 1, BIG, f); fclose(f);
    printf("    30 KB write %lu ms\n", (esp_cpu_get_cycle_count() - t) / 240000);
    t = esp_cpu_get_cycle_count();
    for (int i = 0; i < 40; i++) { f = fopen("/spiffs/churn.bin", "wb"); b[0] = i; fwrite(b, 1, BIG, f); fclose(f); }
    printf("    40 x 30 KB rewrites (GC): %lu ms\n", (esp_cpu_get_cycle_count() - t) / 240000);
    unlink("/spiffs/churn.bin");
    f = fopen("/spiffs/idf.txt", "w"); fprintf(f, "written by IDF spiffs"); fclose(f);
    esp_vfs_spiffs_unregister("spiffs");
}
