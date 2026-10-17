#include "test.h"
#include "spiffs.h"
#include "vfs.h"
#include <string.h>
#include "crc32.h"

extern uint8_t back[32768];
#define BIG 30000                       /* > 103 data pages: needs a 2nd index page */
static uint8_t pat(uint32_t i) { return (uint8_t)(i * 7 + (i >> 9)); }
static bool is_pat(const uint8_t *b, uint32_t from, uint32_t n) { for (uint32_t i = 0; i < n; i++) if (b[i] != pat(from + i)) return false; return true; }

void test_spiffs(void)
{
    const partition_t *p = partition_find(PART_DATA, PART_SUB_SPIFFS, "spiffs");
    CHECK(p != NULL);
    /* 0) a volume written by IDF, if present */
    if (spiffs_mount(p) == 0) {
        char s[64] = { 0 };
        int fd = spiffs_open("/idf.txt", FAT_RDONLY);
        if (fd >= 0) { spiffs_read(fd, s, sizeof s - 1); spiffs_close(fd); printf("    existing volume: idf.txt = \"%s\"\n", s); }
        fd = spiffs_open("/idf_big.bin", FAT_RDONLY);
        if (fd >= 0) { int n = spiffs_read(fd, back, sizeof back); spiffs_close(fd); printf("    existing volume: idf_big.bin %d B, pattern %s\n", n, n == BIG && is_pat(back, 0, BIG) ? "ok" : "BAD"); }
        spiffs_unmount();
    }
    uint32_t t = ccount();
    CHECK(spiffs_format(p) == 0 && spiffs_mount(p) == 0);
    printf("    format 1 MB + mount %lu ms\n", (ccount() - t) / 240000);

    for (uint32_t i = 0; i < BIG; i++) back[i] = pat(i);
    t = ccount();
    int fd = spiffs_open("/data/big.bin", FAT_WRONLY | FAT_CREAT);
    CHECK(fd >= 0 && spiffs_write(fd, back, BIG) == BIG && spiffs_close(fd) == 0);
    uint32_t w = ccount() - t;
    fd = spiffs_open("/hello.txt", FAT_WRONLY | FAT_CREAT);
    CHECK(spiffs_write(fd, "Hello from esp-ndf\n", 19) == 19 && spiffs_close(fd) == 0);

    t = ccount();
    fd = spiffs_open("/data/big.bin", FAT_RDONLY);
    int n = spiffs_read(fd, back, sizeof back);
    uint32_t r = ccount() - t;
    spiffs_close(fd);
    CHECK(n == BIG && is_pat(back, 0, BIG));
    printf("    30 KB file (2 index pages): write %lu ms, read %lu ms\n", w / 240000, r / 240000);

    /* overwrite in the middle (across a page boundary: 251 * 60 = 15060), extend at the end */
    fd = spiffs_open("/data/big.bin", FAT_RDWR);
    spiffs_seek(fd, 15059, FAT_SEEK_SET); spiffs_write(fd, "XYZ", 3);
    spiffs_seek(fd, 0, FAT_SEEK_END); spiffs_write(fd, "END", 3);
    spiffs_seek(fd, 15058, FAT_SEEK_SET); uint8_t q[5]; spiffs_read(fd, q, 5);
    CHECK(q[0] == pat(15058) && q[1] == 'X' && q[3] == 'Z' && q[4] == pat(15062));
    CHECK(spiffs_seek(fd, 0, FAT_SEEK_END) == BIG + 3);
    spiffs_close(fd);
    fd = spiffs_open("/hello.txt", FAT_WRONLY | FAT_APPEND);
    CHECK(spiffs_write(fd, "more\n", 5) == 5 && spiffs_close(fd) == 0);
    uint32_t sz; bool d;
    CHECK(spiffs_stat("/hello.txt", &sz, &d) == 0 && sz == 24 && !d);
    CHECK(spiffs_open("/nope", FAT_RDONLY) == FAT_ENOENT);

    /* churn: 1.2 MB of rewrites through a 1 MB volume forces GC */
    t = ccount();
    for (int i = 0; i < 40; i++) {
        fd = spiffs_open("/churn.bin", FAT_WRONLY | FAT_CREAT | FAT_TRUNC);
        for (uint32_t k = 0; k < BIG; k++) back[k] = pat(k);
        back[0] = i;
        if (spiffs_write(fd, back, BIG) != 30000 || spiffs_close(fd)) { CHECK(0); break; }
    }
    printf("    40 x 30 KB rewrites (GC): %lu ms\n", (ccount() - t) / 240000);

    /* remount: everything from flash */
    CHECK(spiffs_unmount() == 0 && spiffs_mount(p) == 0);
    fd = spiffs_open("/data/big.bin", FAT_RDONLY);
    CHECK(spiffs_read(fd, back, sizeof back) == BIG + 3 && is_pat(back, 0, 15059) && back[15060] == 'Y' &&
          is_pat(back + 15062, 15062, BIG - 15062) && !memcmp(back + BIG, "END", 3));
    spiffs_close(fd);
    fd = spiffs_open("/churn.bin", FAT_RDONLY);
    CHECK(spiffs_read(fd, back, BIG) == BIG && back[0] == 39 && is_pat(back + 1, 1, BIG - 1));
    spiffs_close(fd);
    CHECK(spiffs_unlink("/churn.bin") == 0 && spiffs_stat("/churn.bin", &sz, &d) == FAT_ENOENT);

    CHECK(vfs_register("/spiffs", &vfs_spiffs_ops) == 0);
    int dd = vfs_opendir("/spiffs"); char name[40]; int cnt = 0;
    while (vfs_readdir(dd, name, &sz, &d) == 1) { printf("      %s  %lu\n", name, sz); cnt++; }
    vfs_closedir(dd);
    CHECK(cnt == 2 && vfs_unregister("/spiffs") == 0);
    uint32_t tot, used; spiffs_info(&tot, &used);
    printf("    %lu KB used of %lu KB\n", used / 1024, tot / 1024);
}
