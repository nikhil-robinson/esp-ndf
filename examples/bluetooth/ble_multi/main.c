/* BLE central with two links at once: initiates a connection to each of two advertisers (public addresses NDF_PEER_A / NDF_PEER_B, the IDF peers of the test setup), discovers the GATT server
 * on each, subscribes to its counter characteristic on both and counts the notifications that arrive on each link. Then drops the first link and checks the second keeps running.
 * Prints "multi PASS" / "multi FAIL ...". */
#include "console.h"
#include "sys.h"
#include "cpu.h"
#include "ble_hw.h"
#include "gatt.h"
#include "rf_pll.h"
#include "rf_init.h"
#include <string.h>

#ifndef NDF_PEER_A
#define NDF_PEER_A { 0x4A, 0xC9, 0xE2, 0x7C, 0x58, 0x24 }        /* 24:58:7C:E2:C9:4A (the PEER board) */
#endif
#ifndef NDF_PEER_B
#define NDF_PEER_B { 0x12, 0xC1, 0x5A, 0xF9, 0x30, 0x30 }        /* 30:30:F9:5A:C1:12 (the BOX-3) */
#endif

static volatile uint32_t nn[BLE_NCONN], gp[BLE_NCONN], last[BLE_NCONN]; static volatile uint8_t have[BLE_NCONN];
static void on_notify(uint16_t h, const uint8_t *v, unsigned n)
{
    (void)h; int l = ble_hw_selected(); nn[l]++;
    if (n >= 4) { uint32_t x; memcpy(&x, v, 4); if (have[l] && x != last[l] + 1) gp[l]++; last[l] = x; have[l] = 1; }
}
static int fail(const char *why, int rc) { printf("multi FAIL %s (%d)\n", why, rc); return 1; }

static int subscribe_counter(int link)                          /* discovery on the selected link, subscribe to 0xfff2 */
{
    gattc_svc_t s[10]; unsigned ns = 0; gattc_chr_t c[6]; unsigned nc = 0;
    int rc = gattc_discover(s, 10, &ns);
    if (rc < 0) return fail("discover", rc);
    int app = -1;
    for (unsigned i = 0; i < ns; i++) if (s[i].uuid16 == 0xFFF0) app = (int)i;
    if (app < 0) return fail("service 0xfff0 missing", link);
    rc = gattc_chars(&s[app], c, 6, &nc);
    if (rc < 0 || nc != 2 || c[1].uuid16 != 0xFFF2 || !c[1].cccd) return fail("0xfff2", rc);
    rc = gattc_subscribe(&c[1], 0, on_notify);
    return rc < 0 ? fail("subscribe", rc) : 0;
}

static int run(void)
{
    static const uint8_t peer[2][6] = { NDF_PEER_A, NDF_PEER_B };
    memset((void *)nn, 0, sizeof nn); memset((void *)gp, 0, sizeof gp); memset((void *)have, 0, sizeof have);
    int h[2];
    for (int i = 0; i < 2; i++) {
        h[i] = gattc_connect(peer[i], 0, 15000);
        printf("multi: link %d connected on slot %d (%d links)\n", i, h[i], ble_hw_conn_count());
        if (h[i] < 0) { ble_hw_stats_t st; ble_hw_get_stats(&st); printf("multi: stats events %lu skips %lu aborts %lu packets %lu rx_errors %lu dropped %lu\n", (unsigned long)st.events, (unsigned long)st.skips, (unsigned long)st.aborts, (unsigned long)st.packets, (unsigned long)st.rx_errors, (unsigned long)st.dropped); printf("multi: connect %d failed (%d), trying again\n", i, h[i]); return 1; }          /* the peers may not advertise yet: the main loop starts over */
    }
    if (h[0] == h[1] || ble_hw_conn_count() != 2) return fail("two links", ble_hw_conn_count());
    for (int i = 0; i < 2; i++) { ble_hw_select(h[i]); if (subscribe_counter(h[i])) return 1; }
    uint64_t t0 = time_us();
    while (time_us() - t0 < 6000000) gatt_poll(5000);
    printf("multi: notifications in 6 s: link %d %lu gaps %lu, link %d %lu gaps %lu\n", h[0], (unsigned long)nn[h[0]], (unsigned long)gp[h[0]], h[1], (unsigned long)nn[h[1]], (unsigned long)gp[h[1]]);
    if (nn[h[0]] < 40 || nn[h[1]] < 40 || gp[h[0]] || gp[h[1]] || ble_hw_conn_count() != 2) return fail("notifications on both links", (int)(nn[h[0]] + nn[h[1]]));
    ble_hw_select(h[0]); ble_hw_disconnect();                    /* the first link goes, the second must carry on */
    t0 = time_us(); while (time_us() - t0 < 2000000) gatt_poll(5000);
    uint32_t before = nn[h[1]];
    t0 = time_us(); while (time_us() - t0 < 3000000) gatt_poll(5000);
    printf("multi: after dropping link %d: %d link(s), link %d got %lu more\n", h[0], ble_hw_conn_count(), h[1], (unsigned long)(nn[h[1]] - before));
    if (ble_hw_conn_count() != 1 || ble_hw_link_up(h[0]) || nn[h[1]] - before < 20 || gp[h[1]]) return fail("second link after the first dropped", ble_hw_conn_count());
    ble_hw_select(h[1]); ble_hw_disconnect();
    t0 = time_us(); while (time_us() - t0 < 1500000) gatt_poll(5000);
    printf("multi PASS\n");
    return 0;
}

int main(void)
{
    delay_ms(1000);
    printf("=== ble_multi\n");
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2402000, &r);
    printf("ble_hw_init rc %d\n", ble_hw_init());
    gatts_init("NDF-MULTI");
    for (;;) {
        run();
        for (int k = 0; k < BLE_NCONN; k++) if (ble_hw_link_up(k)) { ble_hw_select(k); ble_hw_disconnect(); }
        uint64_t t0 = time_us(); while (ble_hw_connected() && time_us() - t0 < 3000000) gatt_poll(5000);
        delay_ms(1500);
    }
}
