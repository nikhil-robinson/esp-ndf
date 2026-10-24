/* IDF reads an image made by tools/spiffsgen.py (mounted without format). */
#include <stdio.h>
#include <string.h>
#include "esp_spiffs.h"

void app_main(void)
{
    esp_vfs_spiffs_conf_t c = { .base_path = "/s", .partition_label = "spiffs", .max_files = 4, .format_if_mount_failed = false };
    printf("mount %d\n", esp_vfs_spiffs_register(&c));
    char s[64] = { 0 }; FILE *f = fopen("/s/idf.txt", "r");
    if (f) { fread(s, 1, sizeof s - 1, f); fclose(f); }
    static unsigned char b[32768]; int n = 0, bad = 0;
    f = fopen("/s/idf_big.bin", "rb");
    if (f) { n = fread(b, 1, sizeof b, f); fclose(f); }
    for (int i = 0; i < n; i++) bad += b[i] != (unsigned char)(i * 7 + (i >> 9));
    printf("idf_spiffsgen: idf.txt \"%s\", idf_big.bin %d B, %d wrong\n", s, n, bad);
    printf("idf_spiffsgen done\n");
}
