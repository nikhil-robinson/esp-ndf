/* Wi-Fi MAC spike: what the MAC does without the vendor stack (docs/WIFI_MAC.md). Line commands over the console:
 *   rxinit                  carrier on channel 6 + RX front end + our ring
 *   mac aa:bb:cc:dd:ee:ff   address filter slot 0 + ACK enable (same offsets as the ESP32: filter 0x60033040, ACK en 0x60033064 bit 16)
 *   rx <ms> [v]             receive; per frame: type, length, header words; summary by 802.11 type
 *   rd <addr> [n]           read n words      wr <addr> <val>   write a word
 *   txinit / tx <hex>       carrier + TX front end; send one raw frame (rf_wifi_tx) */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include "rf.h"
#include <stdlib.h>
#include <string.h>

static char line[300];
static int hexbin(const char *h, uint8_t *o, int max) { int n = 0; for (; h[0] && h[1] && n < max; h += 2, n++) { char b[3] = { h[0], h[1], 0 }; o[n] = (uint8_t)strtoul(b, 0, 16); } return n; }
static uint8_t my[6];
static int parse_mac(const char *s, unsigned m[6]) { for (int i = 0; i < 6; i++) { char *e; m[i] = strtoul(s, &e, 16); if (e == s) return -1; s = e + (*e == ':'); } return 0; }
#define R32(a) (*(volatile uint32_t *)(a))

int main(void)
{
    delay_ms(300);
    printf("wifi_mac_spike ready open %d\n", rf_open());
    int k = 0;
    for (;;) {
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch != '\n') { if (k < (int)sizeof line - 1) line[k++] = (char)ch; continue; }
        line[k] = 0; k = 0;
        char *v[6]; int n = 0;
        for (char *p = line; *p && n < 6; ) { while (*p == ' ') *p++ = 0; if (!*p) break; v[n++] = p; while (*p && *p != ' ') p++; }
        if (!n) { printf("ok\n"); continue; }
        if (!strcmp(v[0], "rxinit")) printf("rxinit %d\n", rf_wifi_rx_init());
        else if (!strcmp(v[0], "ring")) { const uint32_t (*r)[4] = rf_wifi_rx_ring(); for (int i = 0; i < 4; i++) printf("d%d %08x %08x %08x %08x\n", i, (unsigned)r[i][0], (unsigned)r[i][1], (unsigned)r[i][2], (unsigned)r[i][3]); }
        else if (!strcmp(v[0], "txinit")) printf("txinit %d\n", rf_wifi_tx_init());
        else if (!strcmp(v[0], "mac") && n > 1) {
            unsigned m[6]; if (parse_mac(v[1], m)) { printf("bad mac\n"); continue; }
            for (int i = 0; i < 6; i++) my[i] = (uint8_t)m[i];
            R32(0x60033040) = m[0] | m[1] << 8 | m[2] << 16 | m[3] << 24; R32(0x60033044) = m[4] | m[5] << 8;
            R32(0x60033060) = 0xFFFFFFFF; R32(0x60033064) = 0x0001FFFF;           /* mask bits + enable (bit 16) */
            printf("mac set %s: %08x %08x ack %08x\n", v[1], (unsigned)R32(0x60033040), (unsigned)R32(0x60033044), (unsigned)R32(0x60033064));
        } else if (!strcmp(v[0], "rx")) {
            uint32_t ms = n > 1 ? strtoul(v[1], 0, 0) : 3000, t0 = ccount(), got = 0, bad = 0, mine = 0, tagged = 0, byt[4] = { 0 };
            static uint8_t b[1700];
            while ((ccount() - t0) / 240000u < ms) {
                int l = rf_wifi_rx(b, sizeof b, 20); if (l <= 0) continue;
                const uint32_t *hh = (const uint32_t *)rf_wifi_rx_header(), dl = (rf_wifi_rx_desc()[0] >> 12) & 0xFFF;
                if ((hh[11] & 0xFFF) + 45 != dl) { bad++; continue; }                  /* no MAC header: a continuation / stale buffer */
                got++; byt[(b[0] >> 2) & 3]++;
                for (int j = 24; j + 9 <= l; j++) if (!memcmp(b + j, "RAWBEACON", 9)) { tagged++; break; }
                static const uint8_t peer[6] = { 0x30, 0x30, 0xf9, 0x5a, 0xc1, 0x11 }, peer2[6] = { 0x30, 0x30, 0xf9, 0x5a, 0xc1, 0x10 };
                if (l >= 16 && (!memcmp(b + 4, my, 6) || !memcmp(b + 10, peer, 6) || !memcmp(b + 10, peer2, 6))) {
                    mine++;
                    if (mine <= 12 && (b[0] & 0xFC) != 0x80) printf("  from/to peer: len %d rssi %d rate %u fc %02x%02x ra %02x%02x%02x%02x%02x%02x\n", l, (int8_t)(hh[0] & 0xFF), (unsigned)((hh[0] >> 8) & 0x1F), b[0], b[1], b[4], b[5], b[6], b[7], b[8], b[9]);
                }
                if (n > 2 && got <= 12) {
                    const uint8_t *h = rf_wifi_rx_header();
                    printf("rx len %3d rssi %d rate %u fc %02x%02x ra %02x:%02x:%02x:%02x:%02x:%02x hdr:", l, (int8_t)(hh[0] & 0xFF), (unsigned)((hh[0] >> 8) & 0x1F), b[0], b[1], b[4], b[5], b[6], b[7], b[8], b[9]);
                    for (int j = 0; j < 12; j++) printf(" %08x", (unsigned)((const uint32_t *)h)[j]);
                    const uint32_t *d = rf_wifi_rx_desc(); printf(" | desc %08x %08x %08x %08x\n", (unsigned)d[0], (unsigned)d[1], (unsigned)d[2], (unsigned)d[3]);
                }
            }
            printf("rx tagged %lu to-me %lu got %lu (headerless %lu) mgmt %lu ctrl %lu data %lu in %lu ms\n", (unsigned long)tagged, (unsigned long)mine, (unsigned long)got, (unsigned long)bad, (unsigned long)byt[0], (unsigned long)byt[1], (unsigned long)byt[2], (unsigned long)ms);
        } else if (!strcmp(v[0], "rd") && n > 1) {
            uint32_t a = strtoul(v[1], 0, 0), c = n > 2 ? strtoul(v[2], 0, 0) : 1;
            for (uint32_t i = 0; i < c; i++) printf("%08x: %08x\n", (unsigned)(a + 4 * i), (unsigned)R32(a + 4 * i));
        } else if (!strcmp(v[0], "wr") && n > 2) R32(strtoul(v[1], 0, 0)) = strtoul(v[2], 0, 0);
        else if (!strcmp(v[0], "tx") && n > 1) {
            static uint8_t f[300]; int l = hexbin(v[1], f, sizeof f);
            printf("tx %d\n", rf_wifi_tx(f, l));
        }
        printf("ok\n");
    }
}
