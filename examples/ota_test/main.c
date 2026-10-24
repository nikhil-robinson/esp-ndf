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

#define STEP  (*(volatile uint32_t *)0x50001F80u)
#define MAGIC (*(volatile uint32_t *)0x50001F84u)
static uint8_t b[4096];

static int copy_self(const partition_t *to, bool corrupt)
{
    const partition_t *from = ota_running();
    uint32_t ram = from ? from->offset : NDF_BOOT_LOAD_ADDR, xip = from ? from->offset + NDF_OTA_XIP_OFF : NDF_XIP_ADDR;
    ota_t o;
    if (ota_begin(&o, to)) return -1;
    for (uint32_t a = 0; a < NDF_OTA_XIP_OFF; a += sizeof b) {          /* RAM image area */
        if (flash_read(ram + a, b, sizeof b)) return -2;
        if (corrupt && a == 0x1000) b[100] ^= 0x55;
        if (ota_write(&o, b, sizeof b)) return -3;
    }
    for (uint32_t a = 0; a < NDF_XIP_SIZE; a += sizeof b) {             /* code window */
        if (flash_read(xip + a, b, sizeof b) || ota_write(&o, b, sizeof b)) return -4;
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
    switch (STEP) {
    case 0:
        if (r) { printf("FAIL expected factory\n"); break; }
        e = copy_self(ota_next(), false);
        printf("%s install into %s (%d)\n", e ? "FAIL" : "PASS", ota_next()->label, e);
        if (e || ota_set_boot(ota_next())) break;
        STEP = 1; sys_restart();
    case 1:
        printf("%s first boot of ota_0 is pending verify\n", r && !strcmp(r->label, "ota_0") && ota_state() == OTA_PENDING_VERIFY ? "PASS" : "FAIL");
        ota_mark_valid();
        printf("%s confirmed (state %d)\n", ota_state() == OTA_VALID ? "PASS" : "FAIL", ota_state());
        e = copy_self(ota_next(), false);
        printf("%s install into %s (%d)\n", e ? "FAIL" : "PASS", ota_next()->label, e);
        if (e || ota_set_boot(ota_next())) break;
        STEP = 2; sys_restart();
    case 2:
        printf("%s ota_1 boots, pending verify; not confirming, reset\n", r && !strcmp(r->label, "ota_1") && ota_state() == OTA_PENDING_VERIFY ? "PASS" : "FAIL");
        STEP = 3; sys_restart();
    case 3:
        printf("%s rollback: unconfirmed ota_1 dropped, back on ota_0 (valid)\n", r && !strcmp(r->label, "ota_0") && ota_state() == OTA_VALID ? "PASS" : "FAIL");
        e = copy_self(ota_next(), true);
        printf("%s corrupted image rejected by ota_end (%d)\n", e == -5 ? "PASS" : "FAIL", e);
        if (ota_set_boot(ota_next())) break;                            /* boot it anyway: the loader must refuse */
        STEP = 4; sys_restart();
    case 4:
        printf("%s loader refused the corrupted image, still ota_0\n", r && !strcmp(r->label, "ota_0") ? "PASS" : "FAIL");
        { const partition_t *od = partition_find(PART_DATA, 0x00, NULL); partition_erase(od, 0, od->size); }
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
