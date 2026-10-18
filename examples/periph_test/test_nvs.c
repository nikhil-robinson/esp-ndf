#include "test.h"
#include "nvs.h"
#include "partition.h"
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
        printf("    ESP-IDF wrote: from_idf=%lx, idf_str=\"%s\", idf_blob %u B (r=%d) %s\n", mark, r1 ? "?" : s, bl, r2,
               !r2 && bl == 5000 && !memcmp(back, big, 5000) ? "matches" : "MISMATCH");
        CHECK(mark == 0xC0FFEE && !r1 && !strcmp(s, "hello from idf") && !r2 && bl == 5000 && !memcmp(back, big, 5000));
        char bs[32];
        nvs_boot_item_t bi[2] = { { .ns = "robot", .key = "from_idf", .type = NVS_TYPE_U32 },
                                  { .ns = "robot", .key = "idf_str", .type = NVS_TYPE_STR, .str = bs, .len = sizeof bs } };
        CHECK(nvs_boot_read("nvs", bi, 2) == 0 && bi[0].result == 0 && bi[0].u32 == 0xC0FFEE &&
              bi[1].result == 0 && !strcmp(bs, "hello from idf"));
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
    /* boot-time reader: same answers without mount or index */
    char bs[32];
    nvs_boot_item_t bi[] = {
        { .ns = "robot", .key = "u8", .type = NVS_TYPE_U8 },
        { .ns = "robot", .key = "i32", .type = NVS_TYPE_I32 },
        { .ns = "robot", .key = "u64", .type = NVS_TYPE_U64 },
        { .ns = "robot", .key = "counter", .type = NVS_TYPE_U32 },         /* 600 versions, newest wins */
        { .ns = "robot", .key = "name", .type = NVS_TYPE_STR, .str = bs, .len = sizeof bs },
        { .ns = "robot", .key = "nokey", .type = NVS_TYPE_U8 },
        { .ns = "nons", .key = "u8", .type = NVS_TYPE_U8 },
        { .ns = "robot", .key = "u8", .type = NVS_TYPE_U16 },             /* wrong type */
    };
    t = ccount();
    CHECK(nvs_boot_read("nvs", bi, 8) == 0);
    uint32_t bt = ccount() - t;
    CHECK(bi[0].result == 0 && bi[0].u8 == 0xAB && bi[1].result == 0 && bi[1].i32 == -123456);
    CHECK(bi[2].result == 0 && bi[2].u64 == 0x1122334455667788ull && bi[3].result == 0 && bi[3].u32 == 599);
    CHECK(bi[4].result == 0 && !strcmp(bs, "esp-ndf robot v1") && bi[4].len == 17);
    CHECK(bi[5].result == NVS_NOT_FOUND && bi[6].result == NVS_NOT_FOUND && bi[7].result == NVS_ERR);
    printf("    nvs_boot_read() 8 keys, %d pages, no mount: %lu us\n", 6, bt / 240);
    int used, freee; nvs_stats(&used, &freee);
    printf("    600 updates: %d failures, %lu us each; entries used %d free %d\n", fails, us / 600, used, freee);
    CHECK(nvs_mount("nvs") == 0);                              /* remount: rebuild from flash */
    CHECK(nvs_get_u32(h, "counter", &ctr) == 0 && ctr == 599);
    bl = sizeof back; CHECK(nvs_get_blob(h, "big10k", back, &bl) == 0 && bl == 10000 && !memcmp(back, big, 10000));
    METRIC("nvs_get_u32()", "%lu cycles", CYCLES(nvs_get_u32(h, "counter", &ctr)));
}

/* Encrypted NVS on "nvs_enc" with keys from "nvs_keys" (both made on the
 * host by tools/nvstool.py: keygen + gen --keyfile). */
void test_nvs_enc(void)
{
    uint8_t key[64];
    if (nvs_read_keys("nvs_keys", key)) { printf("    (no nvs_keys partition content: skipped)\n"); return; }
    uint32_t t = ccount();
    CHECK(nvs_mount_secure("nvs_enc", key) == 0);
    printf("    secure mount %lu us\n", (ccount() - t) / 240);
    nvs_handle_t h;
    uint32_t u; char s[48]; size_t sl = sizeof s, bl;
    if (nvs_open("gen", &h) == 0 && nvs_get_u32(h, "u32", &u) == 0) {      /* host-generated image */
        bl = sizeof back;
        CHECK(nvs_get_str(h, "str", s, &sl) == 0 && nvs_get_blob(h, "blob", back, &bl) == 0);
        printf("    host image: u32 %lu, str \"%s\", blob %u B crc32 %08lx\n", u, s, bl, crc32_le(0, back, bl));
    }
    if (nvs_open("idf", &h) == 0) {
        sl = sizeof s;
        if (nvs_get_str(h, "from_idf", s, &sl) == 0 && nvs_get_u32(h, "ctr", &u) == 0)
            printf("    written by IDF: from_idf \"%s\", ctr %lu\n", s, u);
    }
    CHECK(nvs_open("ndf", &h) == 0);
    pattern(big, sizeof big, 7);
    CHECK(nvs_set_str(h, "secret", "esp-ndf encrypted entry") == 0);
    CHECK(nvs_set_blob(h, "blob6k", big, 6000) == 0);
    int fails = 0;
    t = ccount();
    for (uint32_t i = 0; i < 400; i++) fails += nvs_set_u32(h, "ctr", i) != 0;   /* GC re-encrypts moved entries */
    uint32_t us = (ccount() - t) / 240;
    CHECK(fails == 0);
    CHECK(nvs_mount_secure("nvs_enc", key) == 0);
    sl = sizeof s; bl = sizeof back;
    CHECK(nvs_get_str(h, "secret", s, &sl) == 0 && !strcmp(s, "esp-ndf encrypted entry"));
    CHECK(nvs_get_blob(h, "blob6k", back, &bl) == 0 && bl == 6000 && !memcmp(back, big, 6000));
    CHECK(nvs_get_u32(h, "ctr", &u) == 0 && u == 399);
    /* ciphertext on flash: the plaintext must not appear anywhere */
    const partition_t *p = partition_find(PART_DATA, PART_SUB_NVS, "nvs_enc");
    int plain = 0;
    for (uint32_t off = 0; off < p->size; off += 4096) {
        partition_read(p, off, back, 4096);
        for (int i = 0; i + 8 <= 4096; i++) plain += !memcmp(back + i, "esp-ndf ", 8);
    }
    CHECK(plain == 0);
    /* wrong key: entries fail their CRC, nothing readable */
    key[0] ^= 1;
    CHECK(nvs_mount_secure("nvs_enc", key) == 0 && nvs_get_u32(h, "ctr", &u) == NVS_NOT_FOUND);   /* read-only probe */
    key[0] ^= 1;
    CHECK(nvs_mount_secure("nvs_enc", key) == 0);
    METRIC("nvs_get_u32() encrypted", "%lu cycles", CYCLES(nvs_get_u32(h, "ctr", &u)));
    printf("    400 encrypted updates: %lu us each\n", us / 400);
    CHECK(nvs_mount("nvs") == 0);
}
