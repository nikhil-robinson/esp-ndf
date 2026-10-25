#include "test.h"
#include "pfs.h"
#include "vfs.h"
#include <string.h>

extern uint8_t back[32768];
#define BIG 30000
static uint8_t pat(uint32_t i) { return (uint8_t)(i * 7 + (i >> 9)); }
static bool is_pat(const uint8_t *b, uint32_t from, uint32_t n) { for (uint32_t i = 0; i < n; i++) if (b[i] != pat(from + i)) return false; return true; }
#define MS(c) ((c) / 240000)
#define US(c) ((c) / 240)

/* same workload as bench/idf_littlefs (esp_littlefs on the same partition) */
void test_pfs(void)
{
    const partition_t *p = partition_find(PART_DATA, PART_SUB_SPIFFS, "spiffs");
    CHECK(p != NULL);
    uint32_t t = ccount();
    CHECK(pfs_format(p) == 0);
    uint32_t tf = ccount() - t; t = ccount();
    CHECK(pfs_mount(p) == 0);
    printf("    format %lu ms, mount %lu us\n", MS(tf), US(ccount() - t));

    for (uint32_t i = 0; i < BIG; i++) back[i] = pat(i);
    t = ccount();
    int fd = pfs_open("/data/big.bin", FAT_WRONLY | FAT_CREAT);
    CHECK(fd >= 0 && pfs_write(fd, back, BIG) == BIG && pfs_close(fd) == 0);
    uint32_t w = ccount() - t;
    memset(back, 0, BIG);
    t = ccount();
    fd = pfs_open("/data/big.bin", FAT_RDONLY);
    int n = pfs_read(fd, back, sizeof back);
    uint32_t r = ccount() - t;
    pfs_close(fd);
    CHECK(n == BIG && is_pat(back, 0, BIG));
    printf("    30 KB file: write %lu ms, read %lu us\n", MS(w), US(r));

    fd = pfs_open("/data/big.bin", FAT_RDWR);
    pfs_seek(fd, 15059, FAT_SEEK_SET); pfs_write(fd, "XYZ", 3);
    pfs_seek(fd, 0, FAT_SEEK_END); pfs_write(fd, "END", 3);
    pfs_seek(fd, 15058, FAT_SEEK_SET); uint8_t q[5]; pfs_read(fd, q, 5);
    CHECK(q[0] == pat(15058) && q[1] == 'X' && q[3] == 'Z' && q[4] == pat(15062));
    CHECK(pfs_seek(fd, 0, FAT_SEEK_END) == BIG + 3);
    pfs_close(fd);

    /* log: 64 B lines, each made durable (sync) at once: the robot-log case */
    char line[65];
    t = ccount();
    fd = pfs_open("/log.txt", FAT_WRONLY | FAT_CREAT | FAT_APPEND);
    for (int i = 0; i < 500; i++) {
        snprintf(line, sizeof line, "%05d t=%010lu some sensor values 12.5 33.1 -7.2 ok.........\n", i, (unsigned long)i * 977);
        if (pfs_write(fd, line, 64) != 64 || pfs_sync(fd)) { CHECK(0); break; }
    }
    pfs_close(fd);
    uint32_t lg = ccount() - t;
    printf("    log 500 x 64 B, sync after each: %lu ms (%lu us per line)\n", MS(lg), US(lg) / 500);

    t = ccount();
    for (int i = 0; i < 40; i++) {
        fd = pfs_open("/churn.bin", FAT_WRONLY | FAT_CREAT | FAT_TRUNC);
        for (uint32_t k = 0; k < BIG; k++) back[k] = pat(k);
        back[0] = i;
        if (pfs_write(fd, back, BIG) != BIG || pfs_close(fd)) { CHECK(0); break; }
    }
    printf("    40 x 30 KB rewrites: %lu ms\n", MS(ccount() - t));

    t = ccount();
    CHECK(pfs_unmount() == 0 && pfs_mount(p) == 0);
    printf("    remount after all that: %lu us\n", US(ccount() - t));
    fd = pfs_open("/data/big.bin", FAT_RDONLY);
    CHECK(pfs_read(fd, back, sizeof back) == BIG + 3 && is_pat(back, 0, 15059) && back[15060] == 'Y' &&
          is_pat(back + 15062, 15062, BIG - 15062) && !memcmp(back + BIG, "END", 3));
    pfs_close(fd);
    fd = pfs_open("/churn.bin", FAT_RDONLY);
    CHECK(pfs_read(fd, back, BIG) == BIG && back[0] == 39 && is_pat(back + 1, 1, BIG - 1));
    pfs_close(fd);
    uint32_t sz; bool d;
    CHECK(pfs_stat("/log.txt", &sz, &d) == 0 && sz == 500 * 64);

    /* power cuts on the real flash: torn program / skipped erase after k operations, then "power up" */
    int cuts = 0, lines = 500;
    for (int k = 0; k < 40; k++) {
        pfs_fault_after(k * 3);
        fd = pfs_open("/log.txt", FAT_WRONLY | FAT_APPEND);
        for (int i = 0; i < 20; i++) {
            snprintf(line, sizeof line, "%05d t=%010lu some sensor values 12.5 33.1 -7.2 ok.........\n", lines, (unsigned long)lines * 977);
            if (pfs_write(fd, line, 64) != 64 || pfs_sync(fd)) break;
            lines++;
        }
        pfs_fault_after(-1);
        CHECK(pfs_mount(p) == 0);
        cuts++;
        CHECK(pfs_stat("/log.txt", &sz, &d) == 0 && sz == (uint32_t)lines * 64);
    }
    fd = pfs_open("/log.txt", FAT_RDONLY);
    bool ok = true;
    for (int i = 0; i < lines && ok; i++) {
        char want[65];
        snprintf(want, sizeof want, "%05d t=%010lu some sensor values 12.5 33.1 -7.2 ok.........\n", i, (unsigned long)i * 977);
        ok = pfs_read(fd, line, 64) == 64 && !memcmp(line, want, 64);
    }
    pfs_close(fd);
    CHECK(ok);
    printf("    %d power cuts on flash: log intact, %d lines, every synced line kept\n", cuts, lines);

    CHECK(pfs_unlink("/churn.bin") == 0 && pfs_rename("/log.txt", "/old/log.txt") == 0);
    CHECK(vfs_register("/pfs", &vfs_pfs_ops) == 0);
    int dd = vfs_opendir("/pfs"); char name[64]; int cnt = 0;
    while (vfs_readdir(dd, name, sizeof name, &sz, &d) == 1) { printf("      %s  %lu\n", name, sz); cnt++; }
    vfs_closedir(dd);
    CHECK(cnt == 2 && vfs_unregister("/pfs") == 0);
    uint32_t tot, used; pfs_info(&tot, &used);
    printf("    %lu KB used of %lu KB\n", used / 1024, tot / 1024);
    pfs_unmount();
}

NDF_UNIT(080, pfs);
