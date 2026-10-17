#include "test.h"
#include "nvs.h"
#include "crc32.h"
#include <string.h>

static uint8_t big[10000], back[10000];
static void pattern(uint8_t *b, int n, int seed) { for (int i = 0; i < n; i++) b[i] = (i * 7 + seed) ^ (i >> 5); }

void test_nvs(void)
{
    printf("    crc32_le(0xffffffff, \"123456789\") = %08lx\n", crc32_le(0xFFFFFFFF, "123456789", 9));
    CHECK(nvs_mount("nvs") == 0);
    nvs_handle_t h;
    CHECK(nvs_open("robot", &h) == 0);

    /* 1) data left by the ESP-IDF sibling, if it ran since our last write */
    uint32_t mark;
    if (nvs_get_u32(h, "from_idf", &mark) == 0) {
        char s[32]; size_t sl = sizeof s, bl = sizeof back;
        int r1 = nvs_get_str(h, "idf_str", s, &sl), r2 = nvs_get_blob(h, "idf_blob", back, &bl);
        pattern(big, 5000, 3);
        printf("    ESP-IDF wrote: from_idf=%lx, idf_str=\"%s\", idf_blob %u B %s\n", mark, r1 ? "?" : s, bl,
               !r2 && bl == 5000 && !memcmp(back, big, 5000) ? "matches" : "MISMATCH");
        CHECK(mark == 0xC0FFEE && !r1 && !strcmp(s, "hello from idf") && !r2 && bl == 5000 && !memcmp(back, big, 5000));
    } else {
        printf("    (no ESP-IDF data on flash yet)\n");
    }

    /* 2) our own data set */
    CHECK(nvs_erase_all() == 0);
    CHECK(nvs_open("robot", &h) == 0);
    pattern(big, sizeof big, 1);
    CHECK(nvs_set_u8(h, "u8", 0xAB) == 0);
    CHECK(nvs_set_i32(h, "i32", -123456) == 0);
    CHECK(nvs_set_u64(h, "u64", 0x1122334455667788ull) == 0);
    CHECK(nvs_set_str(h, "name", "esp-ndf robot v1") == 0);
    CHECK(nvs_set_blob(h, "cal3k", big, 3000) == 0);
    CHECK(nvs_set_blob(h, "big10k", big, 10000) == 0);         /* > 1 page: several chunks */
    uint32_t t = ccount();
    int fails = 0;
    for (uint32_t i = 0; i < 600; i++) fails += nvs_set_u32(h, "counter", i) != 0;   /* forces GC */
    uint32_t us = (ccount() - t) / 240;
    uint8_t a; int32_t b; uint64_t c; uint32_t ctr; char s[32]; size_t sl = sizeof s, bl;
    CHECK(nvs_get_u8(h, "u8", &a) == 0 && a == 0xAB);
    CHECK(nvs_get_i32(h, "i32", &b) == 0 && b == -123456);
    CHECK(nvs_get_u64(h, "u64", &c) == 0 && c == 0x1122334455667788ull);
    CHECK(nvs_get_str(h, "name", s, &sl) == 0 && !strcmp(s, "esp-ndf robot v1"));
    bl = sizeof back; CHECK(nvs_get_blob(h, "cal3k", back, &bl) == 0 && bl == 3000 && !memcmp(back, big, 3000));
    bl = sizeof back; CHECK(nvs_get_blob(h, "big10k", back, &bl) == 0 && bl == 10000 && !memcmp(back, big, 10000));
    CHECK(fails == 0 && nvs_get_u32(h, "counter", &ctr) == 0 && ctr == 599);
    int used, freee; nvs_stats(&used, &freee);
    printf("    600 updates: %d failures, %lu us each; entries used %d free %d\n", fails, us / 600, used, freee);
    CHECK(nvs_mount("nvs") == 0);                              /* remount: rebuild from flash */
    CHECK(nvs_get_u32(h, "counter", &ctr) == 0 && ctr == 599);
    bl = sizeof back; CHECK(nvs_get_blob(h, "big10k", back, &bl) == 0 && bl == 10000 && !memcmp(back, big, 10000));
    METRIC("nvs_get_u32()", "%lu cycles", CYCLES(nvs_get_u32(h, "counter", &ctr)));
}
