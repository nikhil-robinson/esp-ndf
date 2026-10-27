/* sta_unit: line commands over the console, driven by tools/wl/test_sta.py.
 *   init | scan | join <ssid> | status | poll <ms> | arp <a.b.c.d> (ARP request from 192.168.4.50, waits 1 s for the reply) */
#include "console.h"
#include "sys.h"
#include "rf.h"
#include "wifi.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t my[6] = { 0x02, 0x4e, 0x44, 0x46, 0x00, 0x02 };
static volatile unsigned eth_n, arp_reply; static uint8_t arp_from[6];
static void on_eth(const uint8_t *f, unsigned n)
{
    eth_n++;
    if (n >= 42 && f[12] == 0x08 && f[13] == 0x06 && f[21] == 2) { arp_reply = 1; memcpy(arp_from, f + 22, 6); }
}

int main(void)
{
    delay_ms(300);
    printf("sta_unit ready open %d\n", rf_open());
    wifi_eth_rx = on_eth;
    static char line[100]; int k = 0;
    for (;;) {
        wifi_poll();
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch != '\n') { if (k < (int)sizeof line - 1) line[k++] = (char)ch; continue; }
        line[k] = 0; k = 0;
        char *v[4]; int n = 0;
        for (char *p = line; *p && n < 4; ) { while (*p == ' ') *p++ = 0; if (!*p) break; v[n++] = p; while (*p && *p != ' ') p++; }
        if (!n) { printf("ok\n"); continue; }
        if (!strcmp(v[0], "init")) printf("init %d\n", wifi_init(my));
        else if (!strcmp(v[0], "scan")) {
            static wifi_ap_t a[16]; unsigned c = 0; uint32_t t0 = time_ms(); wifi_scan(a, 16, &c);
            for (unsigned i = 0; i < c; i++)
                printf("ap %02x:%02x:%02x:%02x:%02x:%02x ch %d rssi %d sec %d ssid %s\n", a[i].bssid[0], a[i].bssid[1], a[i].bssid[2], a[i].bssid[3], a[i].bssid[4], a[i].bssid[5], a[i].chan, a[i].rssi, a[i].sec, a[i].ssid);
            printf("scan %u aps in %lu ms\n", c, (unsigned long)(time_ms() - t0));
        } else if (!strcmp(v[0], "join") && n > 1) { uint32_t t0 = time_ms(); int rc = wifi_sta_join(v[1], 0, 5000); printf("join %d in %lu ms\n", rc, (unsigned long)(time_ms() - t0)); }
        else if (!strcmp(v[0], "status")) printf("status up %d eth %u\n", wifi_sta_up(), eth_n);
        else if (!strcmp(v[0], "poll") && n > 1) { uint32_t t0 = time_ms(), ms = atoi(v[1]); while (time_ms() - t0 < ms) wifi_poll(); printf("status up %d eth %u\n", wifi_sta_up(), eth_n); }
        else if (!strcmp(v[0], "arp") && n > 1) {
            uint8_t a[28] = { 0, 1, 8, 0, 6, 4, 0, 1 }, bc[6] = { 255, 255, 255, 255, 255, 255 }; unsigned ip[4]; const char *q = v[1];
            for (int i = 0; i < 4; i++) { char *e; ip[i] = strtoul(q, &e, 10); q = e + (*e == '.'); }
            memcpy(a + 8, my, 6); a[14] = 192; a[15] = 168; a[16] = 4; a[17] = 50;
            for (int i = 0; i < 4; i++) a[24 + i] = (uint8_t)ip[i];
            arp_reply = 0; int rc = wifi_send_eth(bc, 0x0806, a, 28);
            uint32_t t0 = time_ms(); while (!arp_reply && time_ms() - t0 < 1000) wifi_poll();
            printf("arp tx %d reply %d from %02x:%02x:%02x:%02x:%02x:%02x\n", rc, (int)arp_reply, arp_from[0], arp_from[1], arp_from[2], arp_from[3], arp_from[4], arp_from[5]);
        } else printf("?\n");
        printf("ok\n");
    }
}
