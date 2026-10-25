#include "test.h"
#include "fat.h"
#include <string.h>
#include "crc32.h"
#include "wl.h"
#include "vfs.h"

static uint8_t big[20000];
uint8_t back[32768] __attribute__((aligned(4)));   /* shared with test_spiffs, test_adc_cont */

static void list(const char *path, int depth)
{
    int dd = fat_opendir(path);
    char name[256]; uint32_t size; bool dir;
    while (dd >= 0 && fat_readdir(dd, name, sizeof name, &size, &dir) == 1) {
        printf("      %*s%s%s  %lu\n", depth * 2, "", name, dir ? "/" : "", size);
        if (dir) {
            char sub[300]; strcpy(sub, path); if (strcmp(path, "/")) strcat(sub, "/"); strcat(sub, name);
            list(sub, depth + 1);
        }
    }
    fat_closedir(dd);
}

void test_fat(void)
{
    static bdev_t bd;
    static wl_t wl;
    CHECK(wl_mount(&wl, &bd, partition_find(PART_DATA, PART_SUB_FAT, "fat")) == 0);
    printf("    wl: %lu sectors, dummy at %lu, moves %lu, id %08lx\n", bd.sector_count, wl.st.dummy_pos, wl.st.move_count, wl.st.device_id);
    /* 0) a volume made elsewhere (e.g. IDF fatfsgen.py on the PC), if present */
    if (fat_mount(&bd) == 0) {
        char it[48] = { 0 }; int fi = fat_open("/idf.txt", FAT_RDONLY);
        if (fi >= 0) { fat_read(fi, it, sizeof it - 1); fat_close(fi); printf("    existing volume (IDF WL): idf.txt = \"%s\"\n", it); }
        printf("    existing volume tree:\n"); list("/", 0);
        int fd0 = fat_open("/config files/deep/calibration table.bin", FAT_RDONLY);
        if (fd0 >= 0) {
            int n0 = fat_read(fd0, back, sizeof back); fat_close(fd0); if (n0 < 0) n0 = 0;
            char js[64] = { 0 }; fd0 = fat_open("/config files/robot params.json", FAT_RDONLY);
            fat_read(fd0, js, sizeof js - 1); fat_close(fd0);
            printf("    existing volume (fatfsgen): calibration table.bin %d B crc32 %08lx, json: %s", n0, crc32_le(0, back, n0), js);
            printf("    tree:\n"); list("/", 0);
        }
    }
    uint32_t t = ccount();
    CHECK(fat_format(&bd) == 0);
    uint32_t fmt_ms = (ccount() - t) / 240000;
    uint32_t total, freeb; int bits;
    fat_info(&total, &freeb, &bits);
    printf("    formatted FAT%d in %lu ms: %lu KB, %lu KB free\n", bits, fmt_ms, total / 1024, freeb / 1024);

    int fd = fat_open("/HELLO.TXT", FAT_WRONLY | FAT_CREAT);
    CHECK(fd >= 0 && fat_write(fd, "Hello from esp-ndf\n", 19) == 19 && fat_close(fd) == 0);
    CHECK(fat_mkdir("/data") == 0);
    CHECK(fat_mkdir("/data/sub dir") == 0);
    for (int i = 0; i < (int)sizeof big; i++) big[i] = i * 13 + (i >> 9);
    t = ccount();
    fd = fat_open("/data/long file name.bin", FAT_WRONLY | FAT_CREAT | FAT_TRUNC);
    CHECK(fd >= 0 && fat_write(fd, big, sizeof big) == (int)sizeof big && fat_close(fd) == 0);
    uint32_t w_ms = (ccount() - t) / 240000;
    fd = fat_open("/data/sub dir/note.txt", FAT_WRONLY | FAT_CREAT);
    CHECK(fd >= 0 && fat_write(fd, "nested", 6) == 6 && fat_close(fd) == 0);
    fd = fat_open("/temp.tmp", FAT_WRONLY | FAT_CREAT);
    fat_write(fd, big, 9000); fat_close(fd);
    CHECK(fat_unlink("/temp.tmp") == 0);
    CHECK(fat_unlink("/data") == FAT_ENOTEMPTY);

    CHECK(fat_mount(&bd) == 0);                                 /* remount: everything from flash */
    t = ccount();
    fd = fat_open("/DATA/Long File Name.BIN", FAT_RDONLY);      /* case-insensitive */
    int n = fat_read(fd, back, sizeof back);
    uint32_t r_ms = (ccount() - t) / 240000;
    fat_close(fd);
    CHECK(n == (int)sizeof big && !memcmp(back, big, sizeof big));
    char s[32] = { 0 };
    fd = fat_open("/hello.txt", FAT_RDONLY); fat_read(fd, s, sizeof s - 1); fat_close(fd);
    CHECK(!strcmp(s, "Hello from esp-ndf\n"));
    fd = fat_open("/data/long file name.bin", FAT_RDWR);        /* seek + overwrite in place */
    fat_seek(fd, 15000, FAT_SEEK_SET); fat_write(fd, "XYZ", 3); fat_seek(fd, 14999, FAT_SEEK_SET);
    uint8_t q[5]; fat_read(fd, q, 5); fat_close(fd);
    CHECK(q[0] == big[14999] && q[1] == 'X' && q[3] == 'Z' && q[4] == big[15003]);
    big[15000] = 'X'; big[15001] = 'Y'; big[15002] = 'Z';
    printf("    20 KB file: write %lu ms, read %lu ms\n", w_ms, r_ms);
    printf("    tree:\n"); list("/", 0);
    fat_info(&total, &freeb, &bits);
    printf("    %lu KB free after files; long file name.bin crc32 %08lx\n", freeb / 1024, crc32_le(0, big, sizeof big));

    /* wear levelling: 300 rewrites of one file move the dummy sector ~19 times
     * (more erases per rewrite from FAT/dir updates); data must survive each move */
    uint32_t pos0 = wl.st.dummy_pos;
    for (int i = 0; i < 300; i++) {
        fd = fat_open("/wl.bin", FAT_WRONLY | FAT_CREAT | FAT_TRUNC);
        fat_write(fd, &i, 4); fat_close(fd);
    }
    int v = -1; fd = fat_open("/wl.bin", FAT_RDONLY); fat_read(fd, &v, 4); fat_close(fd);
    CHECK(v == 299 && wl.st.dummy_pos != pos0);
    CHECK(wl_mount(&wl, &bd, partition_find(PART_DATA, PART_SUB_FAT, "fat")) == 0 && fat_mount(&bd) == 0);
    fd = fat_open("/data/long file name.bin", FAT_RDONLY); n = fat_read(fd, back, sizeof back); fat_close(fd);
    CHECK(n == (int)sizeof big && !memcmp(back, big, sizeof big));
    printf("    wl: dummy %lu -> %lu after 300 rewrites, remount ok\n", pos0, wl.st.dummy_pos);

    /* power cut mid FAT-sector rewrite: one copy erased -> rebuilt from the other */
    uint32_t free0; fat_info(&total, &free0, &bits);
    CHECK(fat_sync() == 0);
    for (int copy = 0; copy < 2; copy++) {
        memset(back, 0xFF, 4096);
        CHECK(bd.write(&bd, 1 + copy, back, 1) == 0);         /* FAT1 at sector 1, FAT2 at sector 2 */
        CHECK(fat_mount(&bd) == 0 && fat_repaired() == 1);
        uint32_t f1; fat_info(&total, &f1, &bits);
        fd = fat_open("/data/long file name.bin", FAT_RDONLY); n = fat_read(fd, back, sizeof back); fat_close(fd);
        CHECK(f1 == free0 && n == (int)sizeof big && !memcmp(back, big, sizeof big));
        printf("    FAT%d copy erased: repaired from mirror, %lu KB free, 20 KB file intact\n", copy + 1, f1 / 1024);
    }
    CHECK(fat_mount(&bd) == 0 && fat_repaired() == 0);

    /* VFS: same volume through a "/fat" mount point */
    CHECK(vfs_register("/fat", &vfs_fat_ops) == 0 && vfs_register("/fat", &vfs_fat_ops) == FAT_EEXIST);
    fd = vfs_open("/fat/hello.txt", FAT_RDONLY);
    memset(s, 0, sizeof s);
    CHECK(fd >= 3 && vfs_read(fd, s, sizeof s - 1) == 19 && !strcmp(s, "Hello from esp-ndf\n"));
    CHECK(vfs_unregister("/fat") == FAT_EINVAL && vfs_close(fd) == 0 && vfs_read(fd, s, 1) == FAT_EINVAL);
    CHECK(vfs_open("/nope/x", FAT_RDONLY) == FAT_ENOENT && vfs_open("/fatx/hello.txt", FAT_RDONLY) == FAT_ENOENT);
    uint32_t sz; bool isd;
    CHECK(vfs_stat("/fat/data", &sz, &isd) == 0 && isd && vfs_stat("/fat", &sz, &isd) == 0 && isd);
    int dd = vfs_opendir("/fat/data"), cnt = 0; char nm[256];
    while (vfs_readdir(dd, nm, sizeof nm, &sz, &isd) == 1) cnt++;
    CHECK(dd >= 3 && cnt == 2 && vfs_closedir(dd) == 0);
    t = ccount(); fd = vfs_open("/fat/hello.txt", FAT_RDONLY); uint32_t oc = ccount() - t; vfs_close(fd);
    printf("    vfs_open() incl. FAT lookup %lu cycles; vfs_write(1, ...) -> console\n", oc);
    CHECK(vfs_write(1, "", 0) == 0 && vfs_unregister("/fat") == 0);
}

/* rename / move and timestamps on the volume test_fat left mounted */
#include <sys/time.h>
void test_fat_ext(void)
{
    char s[32] = { 0 }; uint32_t sz; bool d; int fd;
    CHECK(fat_rename("/HELLO.TXT", "/greeting.txt") == 0);
    CHECK(fat_stat("/hello.txt", &sz, &d) == FAT_ENOENT && fat_stat("/greeting.txt", &sz, &d) == 0 && sz == 19);
    CHECK(fat_rename("/greeting.txt", "/data/sub dir/greeting moved.txt") == 0);           /* move between dirs */
    fd = fat_open("/data/sub dir/greeting moved.txt", FAT_RDONLY); fat_read(fd, s, sizeof s - 1); fat_close(fd);
    CHECK(!strcmp(s, "Hello from esp-ndf\n"));
    CHECK(fat_rename("/data/sub dir", "/moved dir") == 0);                                  /* a directory with files */
    CHECK(fat_stat("/moved dir/note.txt", &sz, &d) == 0 && sz == 6 && fat_stat("/data/sub dir", &sz, &d) == FAT_ENOENT);
    CHECK(fat_stat("/moved dir/../data/long file name.bin", &sz, &d) == 0 && sz == 20000);  /* its ".." now = root */
    CHECK(fat_rename("/data", "/data/inside") == FAT_EINVAL);                               /* into itself */
    CHECK(fat_rename("/moved dir/note.txt", "/data/long file name.bin") == FAT_EEXIST);
    static bdev_t *bdp; (void)bdp;
    CHECK(fat_sync() == 0);
    printf("    rename + move (file, dir with files, between dirs), '..' updated, EEXIST / into-itself refused\n");

    struct timeval tv = { .tv_sec = 1761395696 };          /* 2025-10-25 12:34:56 UTC */
    settimeofday(&tv, 0);
    fd = fat_open("/stamp.txt", FAT_WRONLY | FAT_CREAT); fat_write(fd, "t", 1); fat_close(fd);
    time_t mt = 0;
    CHECK(fat_mtime("/stamp.txt", &mt) == 0);
    char b[32]; struct tm g; gmtime_r(&mt, &g); strftime(b, sizeof b, "%F %T", &g);
    printf("    file written at 2025-10-25 12:34:56: mtime %s\n", b);
    CHECK(mt >= 1761395694 && mt <= 1761395698);
    fat_unlink("/stamp.txt");
    fat_rename("/moved dir", "/data/sub dir");                                             /* back, for the next run */
    fat_rename("/data/sub dir/greeting moved.txt", "/HELLO.TXT");
}
