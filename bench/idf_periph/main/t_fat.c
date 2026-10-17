#include "esp_cpu.h"
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
    wl_handle_t wl;
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl("/fat", "fat", &cfg, &wl);
    printf("    mount wear-levelled rw: %s\n    tree:\n", esp_err_to_name(e));
    if (e) return;
    list("/fat", 0);
    FILE *f = fopen("/fat/data/long file name.bin", "rb");
    size_t n = f ? fread(buf, 1, sizeof buf, f) : 0; if (f) fclose(f);
    char s[32] = { 0 }; f = fopen("/fat/hello.txt", "r"); if (f) { fread(s, 1, 31, f); fclose(f); }
    printf("    long file name.bin: %u bytes crc32 %08lx; hello.txt: \"%.18s\"\n", n, esp_rom_crc32_le(0, buf, n), s);
    int v = -1; f = fopen("/fat/wl.bin", "rb"); if (f) { fread(&v, 4, 1, f); fclose(f); }
    printf("    wl.bin = %d\n", v);
    for (int i = 0; i < 200; i++) {             /* IDF moves the dummy sector too */
        f = fopen("/fat/idf.txt", "w"); fprintf(f, "written by IDF WL, pass %d", i); fclose(f);
    }
    uint32_t t0 = esp_cpu_get_cycle_count(); f = fopen("/fat/hello.txt", "r"); uint32_t oc = esp_cpu_get_cycle_count() - t0; fclose(f);
    printf("    fopen() via VFS+FatFs+WL %lu cycles\n", oc);
    esp_vfs_fat_spiflash_unmount_rw_wl("/fat", wl);
}
