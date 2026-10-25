/* OTA end to end over real resets (step in RTC memory): factory installs itself into ota_0 -> ota_0 first boot
 * confirms, installs into ota_1 -> ota_1 does not confirm, reset -> loader must abort it and boot ota_0 -> ota_0
 * installs a corrupted image into ota_1 -> loader must reject it (SHA-256) and boot ota_0 -> otadata erased (factory
 * again). Copy source: the running image (RAM part + its code window). Build with XIP=lib. */
#include "console.h"
#include "ota.h"
#include "flash.h"
#include "sys.h"
#include "system.h"
#include <string.h>
#include "sha.h"
#include "p256.h"

#define STEP  (*(volatile uint32_t *)0x50001F80u)
#define MAGIC (*(volatile uint32_t *)0x50001F84u)
static uint8_t b[4096];

/* TEST-ONLY signing key (a product signs on the host: tools/ota_sign.py; the device holds only the public key) */
static const uint8_t test_priv[32] = { 0x1d, 0x6a, 0x2e, 0x90, 0x44, 0x13, 0x5c, 0xa8, 0x77, 0x02, 0xe5, 0x3b, 0x91, 0x6f, 0x0c, 0xd4,
                                       0x58, 0x29, 0xbf, 0x63, 0x0a, 0xe1, 0x47, 0x9c, 0x35, 0x8e, 0x12, 0x7a, 0xc6, 0x5d, 0x08, 0x31 };
enum { SIGN_OK, SIGN_NONE, SIGN_BAD, SIGN_OLD };

static int copy_self(const partition_t *to, bool corrupt, int sign)
{
    const partition_t *from = ota_running();
    uint32_t ram = from ? from->offset : NDF_BOOT_LOAD_ADDR, xip = from ? from->offset + NDF_OTA_XIP_OFF : NDF_XIP_ADDR;
    ota_t o; sha_ctx_t c; sha_start(&c, SHA256); uint32_t len = 0;
    if (ota_begin(&o, to)) return -1;
    for (uint32_t a = 0; a < NDF_OTA_XIP_OFF; a += sizeof b) {          /* RAM image area */
        if (flash_read(ram + a, b, sizeof b)) return -2;
        if (corrupt && a == 0x1000) b[100] ^= 0x55;
        if (ota_write(&o, b, sizeof b)) return -3;
        sha_update(&c, b, sizeof b); len += sizeof b;
    }
    for (uint32_t a = 0; a < NDF_XIP_SIZE - 4096; a += sizeof b) {      /* code window (last 4 KB: room for the trailer) */
        if (flash_read(xip + a, b, sizeof b) || ota_write(&o, b, sizeof b)) return -4;
        sha_update(&c, b, sizeof b); len += sizeof b;
    }
    if (sign != SIGN_NONE) {                                            /* trailer, as tools/ota_sign.py */
        uint32_t t[3] = { 0x5346444Eu, sign == SIGN_OLD ? 1 : 7, len };
        uint8_t d[32], sig[64];
        sha_update(&c, t, 12); sha_finish(&c, d);
        if (p256_sign(sig, test_priv, d)) return -6;
        if (sign == SIGN_BAD) sig[10] ^= 1;
        memcpy(b, t, 12); memcpy(b + 12, sig, 64);
        if (ota_write(&o, b, 76)) return -7;
    }
    return ota_end(&o) ? -5 : 0;
}

int main(void)
{
    delay_ms(300);
    if (MAGIC != 0x4F544154u) { MAGIC = 0x4F544154u; STEP = 0; }
    const partition_t *r = ota_running();
    printf("ota_test step %lu: running %s, state %d\n", (unsigned long)STEP, r ? r->label : "factory", ota_state());
    int e;
    uint8_t pub[64]; p256_pubkey(pub, test_priv);
    switch (STEP) {
    case 0:
        if (r) { printf("FAIL expected factory\n"); break; }
        e = copy_self(ota_next(), false, SIGN_OK); printf("%s no key set: signed image refused (%d)\n", e == -5 ? "PASS" : "FAIL", e);
        ota_set_key(pub, 5);
        e = copy_self(ota_next(), false, SIGN_NONE); printf("%s unsigned image refused (%d)\n", e == -5 ? "PASS" : "FAIL", e);
        e = copy_self(ota_next(), false, SIGN_BAD); printf("%s bad signature refused (%d)\n", e == -5 ? "PASS" : "FAIL", e);
        e = copy_self(ota_next(), false, SIGN_OLD); printf("%s older version (rollback) refused (%d)\n", e == -5 ? "PASS" : "FAIL", e);
        printf("%s set_boot of a refused slot refused\n", ota_set_boot(ota_next()) ? "PASS" : "FAIL");
        e = copy_self(ota_next(), false, SIGN_OK);
        printf("%s install into %s (%d)\n", e ? "FAIL" : "PASS", ota_next()->label, e);
        if (e || ota_set_boot(ota_next())) break;
        STEP = 1; sys_restart();
    case 1:
        printf("%s first boot of ota_0 is pending verify\n", r && !strcmp(r->label, "ota_0") && ota_state() == OTA_PENDING_VERIFY ? "PASS" : "FAIL");
        ota_mark_valid();
        printf("%s confirmed (state %d)\n", ota_state() == OTA_VALID ? "PASS" : "FAIL", ota_state());
        ota_set_key(pub, 5); e = copy_self(ota_next(), false, SIGN_OK);
        printf("%s install into %s (%d)\n", e ? "FAIL" : "PASS", ota_next()->label, e);
        if (e || ota_set_boot(ota_next())) break;
        STEP = 2; sys_restart();
    case 2:
        printf("%s ota_1 boots, pending verify; not confirming, reset\n", r && !strcmp(r->label, "ota_1") && ota_state() == OTA_PENDING_VERIFY ? "PASS" : "FAIL");
        STEP = 3; sys_restart();
    case 3:
        printf("%s rollback: unconfirmed ota_1 dropped, back on ota_0 (valid)\n", r && !strcmp(r->label, "ota_0") && ota_state() == OTA_VALID ? "PASS" : "FAIL");
        ota_set_key(pub, 5); e = copy_self(ota_next(), true, SIGN_OK);
        printf("%s corrupted image rejected by ota_end (%d)\n", e == -5 ? "PASS" : "FAIL", e);
        printf("%s set_boot of the refused slot refused\n", ota_set_boot(ota_next()) ? "PASS" : "FAIL");
        { const partition_t *od = partition_find(PART_DATA, 0x00, NULL); (void)od; }
        STEP = 4; sys_restart();
    case 4:
        printf("%s still ota_0 after the refused install\n", r && !strcmp(r->label, "ota_0") ? "PASS" : "FAIL");
        { const partition_t *od = partition_find(PART_DATA, 0x00, NULL); partition_erase(od, 0, od->size); }
        REG(0x60008038u) |= 1u << 13;                                  /* next reset cold (ROM + loader), as ota.c sel_put */
        STEP = 5; sys_restart();
    case 9:                                                             /* interop: IDF otatool reads what we write */
        printf("%s ota_set_boot(ota_0) for otatool\n", ota_set_boot(partition_find(PART_APP, 0x10, NULL)) ? "FAIL" : "PASS");
        MAGIC = 0;
        printf("ota_test done\n");
        break;
    case 5:
        printf("%s otadata erased: factory boots\n", r ? "FAIL" : "PASS");
        MAGIC = 0;
        printf("ota_test done\n");
        break;
    }
    for (;;) {}
}
