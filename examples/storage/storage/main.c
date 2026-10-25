/* Storage stack in one place: NVS (boot counter + calibration blob), FAT on
 * wear-levelled flash and SPIFFS, both reached through the VFS. Reboot it and
 * the boot counter goes up; the log file grows by one line per boot. */
#include "nvs.h"
#include "fat.h"
#include "wl.h"
#include "spiffs.h"
#include "vfs.h"
#include "console.h"
#include <string.h>

int main(void)
{
    /* NVS: a counter that survives resets */
    nvs_handle_t h;
    uint32_t boots = 0;
    if (nvs_mount("nvs") || nvs_open("app", &h)) { printf("storage: NVS mount failed\n"); for (;;) ; }
    nvs_get_u32(h, "boots", &boots);
    nvs_set_u32(h, "boots", ++boots);
    printf("storage: boot #%lu\n", boots);

    /* FAT on wear levelling, mounted at /fat (formatted on first use) */
    static wl_t wl; static bdev_t bd;
    int wm = wl_mount(&wl, &bd, partition_find(PART_DATA, PART_SUB_FAT, "fat")), fm = fat_mount(&bd);
    if (wm == 0 && (fm == 0 || (fat_format(&bd) == 0 && fat_mount(&bd) == 0)))
        vfs_register("/fat", &vfs_fat_ops);
    { uint32_t tot, fr; int bits; fat_info(&tot, &fr, &bits);
      printf("storage: FAT%d %lu KB, %lu KB free%s\n", bits, tot / 1024, fr / 1024, fat_repaired() ? " (FAT copy repaired)" : ""); }

    /* SPIFFS at /spiffs (formatted if the partition holds no SPIFFS volume) */
    const partition_t *sp = partition_find(PART_DATA, PART_SUB_SPIFFS, "spiffs");
    if (spiffs_mount(sp) == 0 || (spiffs_format(sp) == 0 && spiffs_mount(sp) == 0))
        vfs_register("/spiffs", &vfs_spiffs_ops);

    /* the same code writes to either filesystem through the VFS */
    static const char *const logs[] = { "/fat/boot.log", "/spiffs/boot.log" };
    for (int i = 0; i < 2; i++) {
        char line[48];
        int n = 0;
        for (const char *s = "boot "; *s; ) line[n++] = *s++;
        char num[12]; int k = 0; uint32_t v = boots;
        do num[k++] = '0' + v % 10; while (v /= 10);
        while (k) line[n++] = num[--k];
        line[n++] = '\n';
        int fd = vfs_open(logs[i], FAT_WRONLY | FAT_CREAT | FAT_APPEND);
        if (fd < 0) { printf("storage: open %s failed (%d)\n", logs[i], fd); continue; }
        int w = vfs_write(fd, line, n), c = vfs_close(fd);
        uint32_t size = 0; bool dir;
        int st = vfs_stat(logs[i], &size, &dir);
        if (w < 0 || c || st) printf("storage: %s: write %d close %d stat %d\n", logs[i], w, c, st);
        else printf("storage: %s is %lu bytes\n", logs[i], size);
    }
    fat_sync();
    for (;;) ;
}
