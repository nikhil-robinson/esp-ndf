/* BLE GATT client on the S3's own link layer: advertises, is connected by a central, then (1.5 s later) uses that same connection as a
 * client toward the central's GATT server: discovers its services and characteristics, reads the device name and 0xFFF1, writes
 * 0xFFF1 and reads it back, subscribes to 0xFFF2 and counts notifications. Prints "gattc PASS" or "gattc FAIL <why>". */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "ble_hw.h"
#include "gatt.h"
#include "rf_pll.h"
#include "rf_init.h"
#include <string.h>

static volatile uint32_t nnotif, gaps, last; static volatile int have;
static void on_notify(uint16_t h, const uint8_t *v, unsigned n)   /* h: the value handle */
{
    (void)h; nnotif++;
    if (n >= 4) { uint32_t x; memcpy(&x, v, 4); if (have && x != last + 1) gaps++; last = x; have = 1; }
}
static int fail(const char *why, int rc) { printf("gattc FAIL %s (%d)\n", why, rc); return 1; }

static int run_client(void)
{
    gattc_svc_t s[10]; unsigned ns = 0;
    int rc = gattc_discover(s, 10, &ns);
    if (rc < 0) return fail("discover", rc);
    printf("services %u:", ns); for (unsigned i = 0; i < ns; i++) printf(" %04x[%u-%u]", s[i].uuid16, s[i].svc_start, s[i].svc_end); printf("\n");
    int gap = -1, app = -1;
    for (unsigned i = 0; i < ns; i++) { if (s[i].uuid16 == 0x1800) gap = (int)i; if (s[i].uuid16 == 0xFFF0) app = (int)i; }
    if (gap < 0 || app < 0) return fail("services 0x1800 / 0xfff0 missing", ns);
    gattc_chr_t c[6]; unsigned nc = 0;
    rc = gattc_chars(&s[gap], c, 6, &nc);
    if (rc < 0 || nc < 1 || c[0].uuid16 != 0x2A00) return fail("device name characteristic", rc);
    uint8_t buf[40]; rc = gattc_read(c[0].value, buf, sizeof buf);
    if (rc <= 0) return fail("read device name", rc);
    printf("device name: %.*s\n", rc, buf);
    rc = gattc_chars(&s[app], c, 6, &nc);
    if (rc < 0 || nc != 2) return fail("0xfff0 characteristics", rc);
    gattc_chr_t *val = &c[0], *cnt = &c[1];
    if (val->uuid16 != 0xFFF1 || cnt->uuid16 != 0xFFF2 || !cnt->cccd) return fail("0xfff1 / 0xfff2 / cccd", cnt->cccd);
    printf("0xfff1 value %u, 0xfff2 value %u cccd %u\n", val->value, cnt->value, cnt->cccd);
    uint8_t pat[12]; for (int i = 0; i < 12; i++) pat[i] = (uint8_t)(0x40 + i);
    rc = gattc_write(val->value, pat, sizeof pat, 1);
    if (rc < 0) return fail("write 0xfff1", rc);
    memset(buf, 0, sizeof buf); rc = gattc_read(val->value, buf, sizeof buf);
    if (rc != 16 || memcmp(buf, pat, 12)) return fail("read back 0xfff1", rc);
    rc = gattc_read(9999, buf, sizeof buf);
    if (rc != -(0x100 + 0x01)) return fail("read of a missing handle must give ATT error 1", rc);
    rc = gattc_subscribe(cnt, 0, on_notify);
    if (rc < 0) return fail("subscribe", rc);
    uint64_t t0 = time_us();
    while (time_us() - t0 < 3000000) gatt_poll(5000);
    printf("notifications in 3 s: %lu, gaps %lu\n", (unsigned long)nnotif, (unsigned long)gaps);
    if (nnotif < 20 || gaps) return fail("notifications", (int)nnotif);
    printf("gattc PASS\n");
    return 0;
}

int main(void)
{
    delay_ms(1000);
    printf("=== ble_gattc\n");
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2402000, &r);
    printf("ble_hw_init rc %d\n", ble_hw_init());
    static const uint8_t addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };
    gatts_init("NDF-GATTC");
    printf("advertise rc %d\n", gatts_advertise(addr, 20000));
    uint64_t up = 0; int done = 0;
    for (;;) {
        gatt_poll(20000);
        if (!ble_hw_connected()) { up = 0; done = 0; continue; }
        if (!up) up = time_us();
        if (!done && time_us() - up > 1500000) { done = 1; run_client(); }
    }
}
