#include "t.h"
#include "esp_vfs_fat.h"
#include "esp_rom_crc.h"
#include <dirent.h>
#include <sys/stat.h>
static uint8_t buf[20000];
static void list(const char *path, int depth)
{
    DIR *d = opendir(path); struct dirent *e;
    while (d && (e = readdir(d))) {
        char sub[300]; snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        struct stat st; stat(sub, &st);
        printf("      %*s%s%s  %ld\n", depth * 2, "", e->d_name, e->d_type == DT_DIR ? "/" : "", (long)st.st_size);
        if (e->d_type == DT_DIR) list(sub, depth + 1);
    }
    if (d) closedir(d);
}
void t_fat(void)
{
    esp_vfs_fat_mount_config_t cfg = { .max_files = 4 };
    esp_err_t e = esp_vfs_fat_spiflash_mount_ro("/fat", "fat", &cfg);
    printf("    mount raw read-only: %s\n    tree:\n", esp_err_to_name(e));
    if (e) return;
    list("/fat", 0);
    FILE *f = fopen("/fat/data/long file name.bin", "rb");
    size_t n = f ? fread(buf, 1, sizeof buf, f) : 0; if (f) fclose(f);
    char s[32] = { 0 }; f = fopen("/fat/hello.txt", "r"); if (f) { fread(s, 1, 31, f); fclose(f); }
    printf("    long file name.bin: %u bytes crc32 %08lx; hello.txt: \"%.18s\"\n", n, esp_rom_crc32_le(0, buf, n), s);
    esp_vfs_fat_spiflash_unmount_ro("/fat", "fat");
}
