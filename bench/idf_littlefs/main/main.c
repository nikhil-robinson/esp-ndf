/* IDF sibling of periph_test "pfs": esp_littlefs (joltwallet 1.14.8) on the same 1 MB partition, same workload.
 * Component (not committed): mkdir -p components/littlefs && cd components/littlefs &&
 *   unzip ~/.espressif/tools/components/joltwallet/littlefs/1.14.8/joltwallet__littlefs-v1.14.8.zip */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_littlefs.h"
#include "esp_timer.h"

#define BIG 30000
static unsigned char back[32768];
static unsigned char pat(unsigned i) { return (unsigned char)(i * 7 + (i >> 9)); }
static esp_vfs_littlefs_conf_t c = { .base_path = "/l", .partition_label = "spiffs", .format_if_mount_failed = true };

void app_main(void)
{
    int64_t t = esp_timer_get_time();
    esp_littlefs_format("spiffs");
    int64_t tf = esp_timer_get_time() - t; t = esp_timer_get_time();
    int r = esp_vfs_littlefs_register(&c);
    printf("    format %lld ms, mount %lld us (%d)\n", tf / 1000, esp_timer_get_time() - t, r);

    for (int i = 0; i < BIG; i++) back[i] = pat(i);
    t = esp_timer_get_time();
    FILE *f = fopen("/l/big.bin", "wb"); fwrite(back, 1, BIG, f); fclose(f);
    int64_t w = esp_timer_get_time() - t;
    memset(back, 0, BIG);
    t = esp_timer_get_time();
    f = fopen("/l/big.bin", "rb"); int n = fread(back, 1, sizeof back, f);
    int64_t rd = esp_timer_get_time() - t;
    fclose(f);
    int bad = 0; for (int i = 0; i < BIG; i++) bad += back[i] != pat(i);
    printf("    30 KB file: write %lld ms, read %lld us (%d B, %d wrong)\n", w / 1000, rd, n, bad);

    char line[80];
    t = esp_timer_get_time();
    f = fopen("/l/log.txt", "a");
    for (int i = 0; i < 500; i++) {
        snprintf(line, sizeof line, "%05d t=%010lu some sensor values 12.5 33.1 -7.2 ok.........\n", i, (unsigned long)i * 977);
        fwrite(line, 1, 64, f); fflush(f); fsync(fileno(f));
    }
    fclose(f);
    int64_t lg = esp_timer_get_time() - t;
    printf("    log 500 x 64 B, sync after each: %lld ms (%lld us per line)\n", lg / 1000, lg / 500);

    t = esp_timer_get_time();
    for (int i = 0; i < 40; i++) {
        for (int k = 0; k < BIG; k++) back[k] = pat(k);
        back[0] = i;
        f = fopen("/l/churn.bin", "wb"); fwrite(back, 1, BIG, f); fclose(f);
    }
    printf("    40 x 30 KB rewrites: %lld ms\n", (esp_timer_get_time() - t) / 1000);

    t = esp_timer_get_time();
    esp_vfs_littlefs_unregister("spiffs");
    r = esp_vfs_littlefs_register(&c);
    printf("    remount after all that: %lld us (%d)\n", esp_timer_get_time() - t, r);
    size_t tot, used; esp_littlefs_info("spiffs", &tot, &used);
    printf("    %u KB used of %u KB\n", (unsigned)used / 1024, (unsigned)tot / 1024);
    printf("idf_littlefs done\n");
}
