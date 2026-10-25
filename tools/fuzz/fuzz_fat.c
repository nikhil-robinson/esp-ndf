/* Host fuzz of drivers/storage/fat.c against corrupted volumes (a FAT image is untrusted: SD cards, USB MSC
 * hosts write it). Builds a valid volume, then per round flips random bytes / words in the boot sector, FATs and
 * directories, mounts and walks everything (dirs, reads, a write, an unlink). ASan/UBSan catch memory errors;
 * alarm() catches endless cluster-chain loops.
 *   gcc -g -O1 -fsanitize=address,undefined -Iinclude -Isoc/esp32s3 tools/fuzz/fuzz_fat.c drivers/storage/fat.c -o /tmp/ff && /tmp/ff 20000 */
#include "fat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define NS 2048
static uint8_t img[NS * 512], disk[NS * 512];
static int rd(bdev_t *b, uint32_t s, void *p, uint32_t n) { (void)b; if (s + n > NS || s + n < s) return -1; memcpy(p, disk + s * 512, n * 512); return 0; }
static int wr(bdev_t *b, uint32_t s, const void *p, uint32_t n) { (void)b; if (s + n > NS || s + n < s) return -1; memcpy(disk + s * 512, p, n * 512); return 0; }
static bdev_t bd = { 512, NS, rd, wr, 0, 0 };
static long round_no;
static void on_alarm(int s) { (void)s; fprintf(stderr, "HANG in round %ld (endless loop on a corrupted volume)\n", round_no); _exit(2); }

static int depth;
static void walk(const char *dir)
{
    if (++depth > 8) { depth--; return; }
    int dd = fat_opendir(dir); char name[300]; uint32_t sz; bool isdir; int n = 0;
    while (dd >= 0 && n++ < 200 && fat_readdir(dd, name, &sz, &isdir) == 1) {
        char path[512]; snprintf(path, sizeof path, "%s/%s", strcmp(dir, "/") ? dir : "", name);
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (isdir) { walk(path); continue; }
        int fd = fat_open(path, FAT_RDONLY); static uint8_t b[4096];
        for (int k = 0; fd >= 0 && k < 64 && fat_read(fd, b, sizeof b) > 0; k++) ;
        if (fd >= 0) fat_close(fd);
        time_t t; fat_mtime(path, &t);
    }
    if (dd >= 0) fat_closedir(dd);
    depth--;
}

int main(int argc, char **argv)
{
    long rounds = argc > 1 ? atol(argv[1]) : 10000;
    srand(argc > 2 ? atoi(argv[2]) : 1);
    signal(SIGALRM, on_alarm);
    memset(disk, 0, sizeof disk);
    if (fat_format(&bd) || fat_mount(&bd)) { puts("format failed"); return 1; }
    fat_mkdir("/a"); fat_mkdir("/a/b");
    static uint8_t data[20000]; for (int i = 0; i < 20000; i++) data[i] = i;
    for (int f = 0; f < 12; f++) { char p[64]; snprintf(p, sizeof p, f < 6 ? "/a/file%d.bin" : "/a/b/a_long_file_name_%d.txt", f);
        int fd = fat_open(p, FAT_WRONLY | FAT_CREAT); fat_write(fd, data, 1000 * (f + 1)); fat_close(fd); }
    fat_unmount();
    memcpy(img, disk, sizeof img);
    for (round_no = 0; round_no < rounds; round_no++) {
        memcpy(disk, img, sizeof disk);
        int nm = 1 + rand() % 16;
        for (int k = 0; k < nm; k++) {
            uint32_t s = rand() % 4 ? rand() % 64 : rand() % NS, o = rand() % 512;     /* metadata mostly */
            switch (rand() % 3) {
            case 0: disk[s * 512 + o] = rand(); break;
            case 1: disk[s * 512 + o] ^= 1 << (rand() % 8); break;
            default: { uint32_t v = rand() % 3 ? (uint32_t)rand() : 0x0FFFFFF0 + rand() % 16; memcpy(disk + s * 512 + (o & ~3u), &v, 4); }
            }
        }
        alarm(5);
        if (fat_mount(&bd) == 0) {
            walk("/");
            int fd = fat_open("/a/new.bin", FAT_WRONLY | FAT_CREAT); if (fd >= 0) { fat_write(fd, data, 5000); fat_close(fd); }
            fat_unlink("/a/file1.bin"); fat_rename("/a/file2.bin", "/a/b/x.bin");
            fat_unmount();
        }
        alarm(0);
    }
    printf("fuzz_fat: %ld rounds, no crash, no hang\n", rounds);
    return 0;
}
