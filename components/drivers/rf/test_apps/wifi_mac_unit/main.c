/* wifi_mac_unit: line commands over the console, driven by tools/wl/test_mac.py.
 *   init | chan <n> | count <ms> <bssid>  (beacons from bssid, other frames, bad-FCS drops) | probe <bssid> <ms>  (broadcast probe request, count probe responses) */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "rf.h"
#include "wifi_mac.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t my[6] = { 0x02, 0x4e, 0x44, 0x46, 0x00, 0x02 };
static int parse_mac(const char *s, uint8_t m[6]) { for (int i = 0; i < 6; i++) { char *e; m[i] = (uint8_t)strtoul(s, &e, 16); if (e == s) return -1; s = e + (*e == ':'); } return 0; }

int main(void)
{
    delay_ms(300);
    printf("wifi_mac_unit ready open %d\n", rf_open());
    static char line[100]; int k = 0;
    for (;;) {
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch != '\n') { if (k < (int)sizeof line - 1) line[k++] = (char)ch; continue; }
        line[k] = 0; k = 0;
        char *v[4]; int n = 0;
        for (char *p = line; *p && n < 4; ) { while (*p == ' ') *p++ = 0; if (!*p) break; v[n++] = p; while (*p && *p != ' ') p++; }
        uint8_t bs[6]; wifi_rx_t r;
        if (!n) { printf("ok\n"); continue; }
        if (!strcmp(v[0], "init")) printf("init %d\n", wifi_mac_init(my));
        else if (!strcmp(v[0], "chan") && n > 1) printf("chan %d\n", wifi_mac_set_channel((uint8_t)atoi(v[1])));
        else if (!strcmp(v[0], "count") && n > 2 && !parse_mac(v[2], bs)) {
            uint32_t ms = atoi(v[1]), t0 = ccount(), bc = 0, other = 0;
            while ((ccount() - t0) / 240000u < ms)
                if (wifi_mac_rx(&r, 5)) { if (r.data[0] == 0x80 && !memcmp(r.data + 10, bs, 6)) bc++; else other++; }
            uint32_t drop; wifi_mac_stats(0, &drop, 0, 0);
            printf("count beacons %lu other %lu drop %lu\n", (unsigned long)bc, (unsigned long)other, (unsigned long)drop);
        } else if (!strcmp(v[0], "probe") && n > 2 && !parse_mac(v[1], bs)) {
            uint8_t f[32] = { 0x40, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, ie[8] = { 0, 0, 1, 4, 0x82, 0x84, 0x8b, 0x96 };
            memcpy(f + 10, my, 6); memset(f + 16, 0xff, 6); memcpy(f + 24, ie, 8);
            int rc = wifi_mac_tx(f, 32), got = 0; uint32_t ms = atoi(v[2]), t0 = ccount();
            while ((ccount() - t0) / 240000u < ms)
                if (wifi_mac_rx(&r, 5) && r.data[0] == 0x50 && !memcmp(r.data + 4, my, 6) && !memcmp(r.data + 10, bs, 6)) got++;
            printf("probe tx %d responses %d\n", rc, got);
        } else printf("?\n");
        printf("ok\n");
    }
}
