/* Host fuzz of the flash filesystems against corrupted flash: SPIFFS (drivers/storage/spiffs.c) and NVS
 * (drivers/storage/nvs.c). A valid volume is built, then each round corrupts random bytes, remounts and reads
 * everything back. ASan/UBSan catch memory errors, alarm() endless loops.
 *   gcc -g -O1 -fsanitize=address,undefined -Iinclude -Isoc/esp32s3 tools/fuzz/fuzz_flashfs.c \
 *       drivers/storage/spiffs.c drivers/storage/nvs.c lib/crc32.c -o /tmp/fz && /tmp/fz spiffs|nvs 20000 */
#include "spiffs.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define FSZ (256 * 1024)
static uint8_t nor[FSZ], img[FSZ];
static partition_t P = { .type = PART_DATA, .subtype = PART_SUB_SPIFFS, .offset = 0, .size = FSZ, .label = "x" };
int partition_read(const partition_t *p, uint32_t o, void *b, size_t n) { (void)p; if (o > FSZ || n > FSZ - o) return -1; memcpy(b, nor + o, n); return 0; }
int partition_write(const partition_t *p, uint32_t o, const void *b, size_t n) { (void)p; if (o > FSZ || n > FSZ - o) return -1; for (size_t i = 0; i < n; i++) nor[o + i] &= ((const uint8_t *)b)[i]; return 0; }
int partition_erase(const partition_t *p, uint32_t o, size_t n) { (void)p; if (o % 4096 || n % 4096 || o > FSZ || n > FSZ - o) return -1; memset(nor + o, 0xFF, n); return 0; }
const partition_t *partition_find(uint8_t t, uint8_t s, const char *l) { (void)t; (void)l; P.subtype = s; return &P; }
void aes_xts(const uint8_t key[64], uint32_t tweak, bool enc, const void *in, void *out, size_t n) { (void)key; (void)tweak; (void)enc; memmove(out, in, n); }
void aes_init(void) {}
static long rnd; static void on_alarm(int s) { (void)s; fprintf(stderr, "HANG round %ld\n", rnd); _exit(2); }

static void corrupt(void)
{
    int nm = 1 + rand() % 24;
    for (int k = 0; k < nm; k++) {
        uint32_t o = rand() % FSZ;
        switch (rand() % 3) {
        case 0: nor[o] = rand(); break;
        case 1: nor[o] ^= 1 << (rand() % 8); break;
        default: { uint32_t v = rand() % 2 ? (uint32_t)rand() : 0xFFFFFFFFu - rand() % 64; memcpy(nor + (o & ~3u), &v, 4); }
        }
    }
}

static void spiffs_round(void)
{
    if (spiffs_mount(&P)) return;
    int dd = spiffs_opendir("/"); char name[64]; uint32_t sz; bool d; static uint8_t b[8192];
    for (int n = 0; dd >= 0 && n < 100 && spiffs_readdir(dd, name, sizeof name, &sz, &d) == 1; n++) {
        char path[80]; snprintf(path, sizeof path, "/%s", name);
        int fd = spiffs_open(path, FAT_RDONLY);
        for (int k = 0; fd >= 0 && k < 16 && spiffs_read(fd, b, sizeof b) > 0; k++) ;
        if (fd >= 0) spiffs_close(fd);
    }
    if (dd >= 0) spiffs_closedir(dd);
    int fd = spiffs_open("/new.bin", FAT_WRONLY | FAT_CREAT | FAT_TRUNC); if (fd >= 0) { spiffs_write(fd, b, 3000); spiffs_close(fd); }
    spiffs_unlink("/f3.bin"); spiffs_rename("/f4.bin", "/g4.bin");
    uint32_t t, u; spiffs_info(&t, &u);
    spiffs_unmount();
}

static void nvs_round(void)
{
    if (nvs_mount("x")) return;
    nvs_handle_t h; char s[64]; static uint8_t b[4096]; size_t n; uint32_t v;
    for (int ns = 0; ns < 3; ns++) {
        char nsn[8]; snprintf(nsn, sizeof nsn, "ns%d", ns);
        if (nvs_open(nsn, &h)) continue;
        for (int k = 0; k < 20; k++) {
            char key[16]; snprintf(key, sizeof key, "k%d", k);
            nvs_get_u32(h, key, &v);
            n = sizeof s; nvs_get_str(h, key, s, &n);
            n = sizeof b; nvs_get_blob(h, key, b, &n);
        }
        nvs_set_u32(h, "fresh", 1); nvs_set_blob(h, "fb", b, 1500); nvs_erase_key(h, "k3");
    }
    int used, fr; nvs_stats(&used, &fr);
    nvs_boot_item_t it[4] = { 0 }; (void)it;
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "spiffs";
    long rounds = argc > 2 ? atol(argv[2]) : 5000;
    srand(argc > 3 ? atoi(argv[3]) : 1);
    signal(SIGALRM, on_alarm);
    memset(nor, 0xFF, FSZ);
    static uint8_t data[9000]; for (int i = 0; i < 9000; i++) data[i] = i * 7;
    if (!strcmp(which, "spiffs")) {
        spiffs_format(&P); spiffs_mount(&P);
        for (int f = 0; f < 10; f++) { char p[32]; snprintf(p, sizeof p, "/f%d.bin", f);
            int fd = spiffs_open(p, FAT_WRONLY | FAT_CREAT); spiffs_write(fd, data, 500 * (f + 1)); spiffs_close(fd); }
        spiffs_unmount();
    } else {
        P.subtype = PART_SUB_NVS; nvs_mount("x");
        nvs_handle_t h;
        for (int ns = 0; ns < 3; ns++) { char nsn[8]; snprintf(nsn, sizeof nsn, "ns%d", ns); nvs_open(nsn, &h);
            for (int k = 0; k < 20; k++) { char key[16]; snprintf(key, sizeof key, "k%d", k);
                if (k % 3 == 0) nvs_set_u32(h, key, k); else if (k % 3 == 1) nvs_set_str(h, key, "hello world string");
                else nvs_set_blob(h, key, data, 100 + 300 * k); } }
    }
    memcpy(img, nor, FSZ);
    for (rnd = 0; rnd < rounds; rnd++) {
        memcpy(nor, img, FSZ); corrupt();
        alarm(5);
        if (!strcmp(which, "spiffs")) spiffs_round(); else nvs_round();
        alarm(0);
    }
    printf("fuzz_%s: %ld rounds, no crash, no hang\n", which, rounds);
    return 0;
}
